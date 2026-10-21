#pragma once

#include <bg/frame/frame.hpp>
#include <bg/frame/literal.hpp>

#include <arrow/api.h>
#include <arrow/array/data.h>
#include <arrow/array/util.h>
#include <arrow/buffer.h>
#include <arrow/util/bit_util.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bg::frame {

namespace arrow_details {

using scalar_runtime_factory = std::unique_ptr<ScalarRuntimeType> (*)();
const ScalarRuntimeType &canonical_scalar_runtime(const ScalarType &type, const std::type_info &identity,
                                                  scalar_runtime_factory factory);

template <cell_type T> std::shared_ptr<arrow::DataType> data_type() {
    if constexpr (std::same_as<T, bool>)
        return arrow::boolean();
    else if constexpr (std::same_as<T, i64>)
        return arrow::int64();
    else
        return arrow::float64();
}

template <cell_type T> constexpr auto arrow_type_id = [] {
    if constexpr (std::same_as<T, bool>)
        return arrow::Type::BOOL;
    else if constexpr (std::same_as<T, i64>)
        return arrow::Type::INT64;
    else
        return arrow::Type::DOUBLE;
}();

inline arrow::Status check_length(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        return arrow::Status::CapacityError("frame value exceeds Arrow's signed 64-bit length limit");
    return arrow::Status::OK();
}

class ArrowRuntimeFrameStorage;
FrameReadStorage frame_read_storage(const ArrowRuntimeFrameStorage &storage, const RuntimeType *runtime);
const ScalarRuntimeType &runtime_schema_frame_scalar_runtime(const FrameType &type);
void register_arrow_column_literal_runtime(const ScalarRuntimeType &runtime);

template <typename Semantic> class ArrowScalarRuntimeBase : public ScalarRuntimeType {
  protected:
    using ScalarRuntimeType::ScalarRuntimeType;
};

template <cell_type T> arrow::Status check_array_buffers(const std::shared_ptr<arrow::Array> &array) {
    if (!array || array->type_id() != arrow_type_id<T>)
        return arrow::Status::TypeError("Arrow column has an unsupported physical type");
    const auto &buffers = array->data()->buffers;
    if (buffers.size() < 2)
        return arrow::Status::Invalid("Arrow array has fewer than two buffers");
    for (const auto &buffer : buffers)
        if (buffer && !buffer->is_cpu())
            return arrow::Status::NotImplemented("Arrow buffers must be on the CPU");
    return arrow::Status::OK();
}

template <cell_type T> arrow::Status check_chunked_buffers(const std::shared_ptr<arrow::ChunkedArray> &column) {
    if (!column || column->type()->id() != arrow_type_id<T>)
        return arrow::Status::TypeError("Arrow column type does not match the frame schema");
    for (const auto &chunk : column->chunks())
        ARROW_RETURN_NOT_OK(check_array_buffers<T>(chunk));
    return arrow::Status::OK();
}

template <cell_type T> arrow::Status validate_chunked(const std::shared_ptr<arrow::ChunkedArray> &column) {
    ARROW_RETURN_NOT_OK(check_chunked_buffers<T>(column));
    ARROW_RETURN_NOT_OK(column->ValidateFull());
    return arrow::Status::OK();
}

template <cell_type T> class ArrowColumnReadSource final : public ColumnReadSource<T> {
  public:
    ArrowColumnReadSource() = default;
    ArrowColumnReadSource(bg::details::FieldProbe &probe) noexcept { probe.record(this); }
    explicit ArrowColumnReadSource(std::shared_ptr<arrow::ChunkedArray> column) { bind(std::move(column)); }

    void bind(std::shared_ptr<arrow::ChunkedArray> column) {
        column_ = std::move(column);
        starts_.clear();
        starts_.reserve(column_ ? column_->num_chunks() + 1 : 0);
        auto position = std::size_t{};
        if (!column_)
            return;
        starts_.push_back(0);
        for (const auto &chunk : column_->chunks()) {
            position += static_cast<std::size_t>(chunk->length());
            starts_.push_back(position);
        }
    }

    std::size_t size() const noexcept override {
        return column_ == nullptr ? 0 : static_cast<std::size_t>(column_->length());
    }

    std::optional<T> cell(std::size_t position) const override {
        if (position >= size())
            throw std::out_of_range{"Arrow column cell position"};
        const auto end = std::upper_bound(starts_.begin(), starts_.end(), position);
        const auto index = static_cast<std::size_t>(std::distance(starts_.begin(), end) - 1);
        const auto &chunk = column_->chunk(static_cast<int>(index));
        const auto local = position - starts_[index];
        if (chunk->IsNull(static_cast<std::int64_t>(local)))
            return std::nullopt;
        const auto value_position = static_cast<std::size_t>(chunk->offset()) + local;
        if constexpr (std::same_as<T, bool>) {
            const auto *values = chunk->data()->buffers[1]->data();
            return arrow::bit_util::GetBit(values, static_cast<std::int64_t>(value_position));
        } else {
            const auto *values = chunk->data()->buffers[1]->data_as<T>();
            return values[value_position];
        }
    }

    void visit_batches(void *target, typename ColumnReadSource<T>::batch_visitor visit) const override {
        if (column_ == nullptr)
            return;
        for (const auto &chunk : column_->chunks()) {
            const auto length = static_cast<std::size_t>(chunk->length());
            if (length == 0)
                continue;
            const auto &buffers = chunk->data()->buffers;
            const auto offset = static_cast<std::uint64_t>(chunk->offset());
            const auto all_valid = chunk->null_count() == 0;
            const auto all_invalid = chunk->null_count() == chunk->length();
            auto validity = ColumnBatch<T>::Validity::all_valid();
            if (all_invalid) {
                validity = ColumnBatch<T>::Validity::all_invalid();
            } else if (!all_valid) {
                validity = ColumnBatch<T>::Validity::from_bitmap(buffers[0]->data(), offset);
            }
            if constexpr (std::same_as<T, bool>) {
                const auto *values = buffers[1] == nullptr ? nullptr : buffers[1]->data() + offset / 8;
                std::invoke(visit, target, ColumnBatch<T>{offset, length, values, offset % 8, validity});
            } else {
                const auto *values = buffers[1] == nullptr
                                         ? nullptr
                                         : buffers[1]->data_as<T>() + static_cast<std::size_t>(offset);
                std::invoke(visit, target, ColumnBatch<T>{offset, length, values, 0, validity});
            }
        }
    }

  private:
    std::shared_ptr<arrow::ChunkedArray> column_;
    std::vector<std::size_t> starts_;
};

template <typename Storage, typename Value> struct ArrowScalarValueRead;

template <cell_type T> struct ArrowScalarValueRead<ArrowColumnReadSource<T>, ColumnView<T>> {
    static ColumnView<T> read(const ArrowColumnReadSource<T> &source) { return ColumnView<T>{source}; }
};

template <cell_type T> arrow::Result<std::shared_ptr<arrow::Array>> make_array(const ColumnBatch<T> &batch) {
    ARROW_RETURN_NOT_OK(check_length(batch.size));
    constexpr auto packed_values = std::same_as<T, bool>;
    if constexpr (packed_values) {
        if (batch.value_bit_offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return arrow::Status::CapacityError("Arrow value buffer exceeds the signed 64-bit size limit");
    }
    const auto value_bit_offset = [&] {
        if constexpr (packed_values)
            return static_cast<std::int64_t>(batch.value_bit_offset);
        else
            return std::int64_t{};
    }();
    if constexpr (packed_values) {
        if (batch.size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() - value_bit_offset))
            return arrow::Status::CapacityError("Arrow value buffer exceeds the signed 64-bit size limit");
    } else if (batch.size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / sizeof(T))) {
        return arrow::Status::CapacityError("Arrow value buffer exceeds the signed 64-bit size limit");
    }

    const auto bitmap_bits = batch.size + static_cast<std::size_t>(value_bit_offset);
    const auto bitmap_bytes = bitmap_bits / 8 + (bitmap_bits % 8 != 0);
    const auto value_bytes = [&] {
        if constexpr (packed_values)
            return bitmap_bytes;
        else
            return batch.size * sizeof(T);
    }();
    const auto byte_size = static_cast<std::int64_t>(value_bytes);
    auto validity_bytes = std::vector<std::uint8_t>(bitmap_bytes, 0);
    auto null_count = std::int64_t{};
    for (auto position = std::size_t{}; position != batch.size; ++position) {
        if (batch.is_valid(position)) {
            arrow::bit_util::SetBit(validity_bytes.data(), value_bit_offset + static_cast<std::int64_t>(position));
        } else {
            ++null_count;
        }
    }

    std::shared_ptr<arrow::Buffer> validity;
    if (null_count != 0)
        validity = arrow::Buffer::FromVector(std::move(validity_bytes));

    std::shared_ptr<arrow::Buffer> values;
    if (batch.values != nullptr) {
        values = std::make_shared<arrow::Buffer>(reinterpret_cast<const std::uint8_t *>(batch.values), byte_size);
    } else {
        if (null_count != static_cast<std::int64_t>(batch.size))
            return arrow::Status::Invalid("Arrow export found valid cells without a value buffer");
        ARROW_ASSIGN_OR_RAISE(auto allocation, arrow::AllocateBuffer(byte_size));
        if (byte_size != 0)
            std::memset(allocation->mutable_data(), 0, static_cast<std::size_t>(byte_size));
        values = std::shared_ptr<arrow::Buffer>{std::move(allocation)};
    }

    auto data = arrow::ArrayData::Make(data_type<T>(), static_cast<std::int64_t>(batch.size),
                                       {std::move(validity), std::move(values)}, null_count, value_bit_offset);
    return arrow::MakeArray(data);
}

template <cell_type T>
arrow::Result<std::shared_ptr<arrow::ChunkedArray>> export_column(const ColumnView<T> &column, bool copy_values) {
    ARROW_RETURN_NOT_OK(check_length(column.size()));
    if (copy_values) {
        if constexpr (!std::same_as<T, bool>) {
            if (column.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / sizeof(T)))
                return arrow::Status::CapacityError("Arrow value buffer exceeds the signed 64-bit size limit");
        }

        using DenseValue = std::conditional_t<std::same_as<T, bool>, std::uint8_t, T>;
        auto dense_values = std::vector<DenseValue>{};
        if constexpr (std::same_as<T, bool>)
            dense_values.resize(column.size() / 8 + (column.size() % 8 != 0));
        else
            dense_values.resize(column.size());
        auto validity_bytes = std::vector<std::uint8_t>(column.size() / 8 + (column.size() % 8 != 0), 0);
        auto position = std::size_t{};
        auto null_count = std::int64_t{};
        auto status = arrow::Status::OK();
        column.batches([&](const auto &batch) {
            if (!status.ok())
                return;
            if (batch.size > column.size() - position) {
                status = arrow::Status::Invalid("column batches exceed the column view length");
                return;
            }
            for (auto offset = std::size_t{}; offset != batch.size; ++offset, ++position) {
                if (!batch.is_valid(offset)) {
                    ++null_count;
                    continue;
                }
                if (!batch.has_values()) {
                    status = arrow::Status::Invalid("column batch has valid cells without a value buffer");
                    return;
                }
                if constexpr (std::same_as<T, bool>) {
                    if (batch.value_at(offset))
                        arrow::bit_util::SetBit(dense_values.data(), static_cast<std::int64_t>(position));
                } else {
                    dense_values[position] = batch.value_at(offset);
                }
                arrow::bit_util::SetBit(validity_bytes.data(), static_cast<std::int64_t>(position));
            }
        });
        ARROW_RETURN_NOT_OK(status);
        if (position != column.size())
            return arrow::Status::Invalid("column batch lengths do not match the column view");

        std::shared_ptr<arrow::Buffer> validity;
        if (null_count != 0)
            validity = arrow::Buffer::FromVector(std::move(validity_bytes));
        auto values = arrow::Buffer::FromVector(std::move(dense_values));
        auto array = arrow::MakeArray(arrow::ArrayData::Make(
            data_type<T>(), static_cast<std::int64_t>(column.size()),
            {std::move(validity), std::move(values)}, null_count));
        return arrow::ChunkedArray::Make({std::move(array)}, data_type<T>());
    }

    auto chunks = std::vector<std::shared_ptr<arrow::Array>>{};
    auto status = arrow::Status::OK();
    column.batches([&](const auto &batch) {
        if (!status.ok())
            return;
        auto result = make_array(batch);
        if (!result.ok()) {
            status = result.status();
            return;
        }
        chunks.push_back(std::move(result).ValueUnsafe());
    });
    ARROW_RETURN_NOT_OK(status);
    auto result = arrow::ChunkedArray::Make(std::move(chunks), data_type<T>());
    if (!result.ok())
        return result.status();
    if (result.ValueUnsafe()->length() != static_cast<std::int64_t>(column.size()))
        return arrow::Status::Invalid("column batch lengths do not match the column view");
    return std::move(result).ValueUnsafe();
}

inline arrow::Result<std::shared_ptr<arrow::ChunkedArray>> export_column(const ColumnView<> &column,
                                                                          bool copy_values) {
    if (std::addressof(column.cell_type()) == std::addressof(cpp_scalar_type<bool>()))
        return export_column(column.view<bool>(), copy_values);
    if (std::addressof(column.cell_type()) == std::addressof(cpp_scalar_type<i64>()))
        return export_column(column.view<i64>(), copy_values);
    if (std::addressof(column.cell_type()) == std::addressof(cpp_scalar_type<double>()))
        return export_column(column.view<double>(), copy_values);
    return arrow::Status::Invalid("frame column has an unsupported cell type");
}

template <bool CopyValues, template <template <typename, auto...> typename> typename Schema>
arrow::Result<std::shared_ptr<arrow::Table>> export_runtime_frame(const FrameView<Schema> &frame) {
    ARROW_RETURN_NOT_OK(check_length(frame.size()));
    auto fields = std::vector<std::shared_ptr<arrow::Field>>{};
    auto columns = std::vector<std::shared_ptr<arrow::ChunkedArray>>{};
    const auto &schema = frame.schema();
    fields.reserve(schema.field_count());
    columns.reserve(schema.field_count());
    for (auto index = std::size_t{}; index != schema.field_count(); ++index) {
        const auto &field = schema.field(index);
        const auto source = frame.column(index);
        auto column = std::shared_ptr<arrow::ChunkedArray>{};
        if (&source.cell_type() == std::addressof(cpp_scalar_type<bool>())) {
            fields.push_back(arrow::field(std::string{field.name.text()}, data_type<bool>(), true));
        } else if (&source.cell_type() == std::addressof(cpp_scalar_type<i64>())) {
            fields.push_back(arrow::field(std::string{field.name.text()}, data_type<i64>(), true));
        } else if (&source.cell_type() == std::addressof(cpp_scalar_type<double>())) {
            fields.push_back(arrow::field(std::string{field.name.text()}, data_type<double>(), true));
        } else {
            return arrow::Status::Invalid("frame column has an unsupported cell type");
        }
        ARROW_ASSIGN_OR_RAISE(column, export_column(source, CopyValues));
        if (column->length() != static_cast<std::int64_t>(frame.size()))
            return arrow::Status::Invalid("frame column length does not match the frame row count");
        columns.push_back(std::move(column));
    }
    return arrow::Table::Make(arrow::schema(std::move(fields)), std::move(columns),
                              static_cast<std::int64_t>(frame.size()));
}

template <typename Semantic, typename Storage, typename Value>
class ArrowScalarRuntime final : public ArrowScalarRuntimeBase<Semantic> {
    using Base = ArrowScalarRuntimeBase<Semantic>;

  public:
    ArrowScalarRuntime()
        : Base{scalar_type_traits<Semantic>::type(), sizeof(Storage), alignof(Storage), false, true, false} {}

    void read_value(ScalarDataMode mode, std::uintptr_t data, void *destination) const override {
        static_cast<void>(mode);
        auto value = ArrowScalarValueRead<Storage, Value>::read(*reinterpret_cast<const Storage *>(data));
        std::construct_at(static_cast<Value *>(destination), std::move(value));
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<Storage *>(destination));
    }
    void copy_construct(void *destination, const void *source, allocator &) const override {
        std::construct_at(static_cast<Storage *>(destination), *static_cast<const Storage *>(source));
    }
    std::size_t hash(const void *) const noexcept override { return 0; }
    bool equals_same_type(const void *value, const ScalarData &other) const override {
        const auto left = ArrowScalarValueRead<Storage, Value>::read(*static_cast<const Storage *>(value));
        return column_values_equal(left, bg::details::scalar_data_value<Semantic>(other));
    }
    void destroy(void *value, allocator &) const noexcept override { std::destroy_at(static_cast<Storage *>(value)); }
    bool accepts_input(const ScalarRuntimeType &source) const noexcept override { return &source == this; }

  protected:
    void do_construct_from_input(void *destination, const ScalarData &source, allocator &) const override {
        std::construct_at(static_cast<Storage *>(destination), *static_cast<const Storage *>(source.data()));
    }
    void do_assign_from_input(void *destination, const ScalarData &source) const override {
        auto replacement = *static_cast<const Storage *>(source.data());
        *static_cast<Storage *>(destination) = std::move(replacement);
    }
};

template <typename Semantic, typename Storage, typename Value>
const ArrowScalarRuntime<Semantic, Storage, Value> &scalar_runtime() {
    return static_cast<const ArrowScalarRuntime<Semantic, Storage, Value> &>(canonical_scalar_runtime(
        scalar_type_traits<Semantic>::type(), typeid(Storage), []() -> std::unique_ptr<ScalarRuntimeType> {
            return std::make_unique<ArrowScalarRuntime<Semantic, Storage, Value>>();
        }));
}

template <cell_type T>
const ArrowScalarRuntime<Column<T>, ArrowColumnReadSource<T>, ColumnView<T>> &column_scalar_runtime() {
    static_cast<void>(cpp_scalar_runtime_type<Column<T>>());
    const auto &runtime = scalar_runtime<Column<T>, ArrowColumnReadSource<T>, ColumnView<T>>();
    register_arrow_column_literal_runtime(runtime);
    return runtime;
}

using ArrowRuntimeFrameColumn =
    std::variant<ArrowColumnReadSource<bool>, ArrowColumnReadSource<i64>, ArrowColumnReadSource<double>>;

class ArrowRuntimeFrameStorage {
  public:
    ArrowRuntimeFrameStorage() = default;
    ArrowRuntimeFrameStorage(std::shared_ptr<arrow::Table> input, const bg::Schema &input_schema)
        : table{std::move(input)}, schema{std::addressof(input_schema)},
          size{static_cast<std::size_t>(table->num_rows())} {
        columns.reserve(schema->field_count());
        for (auto index = std::size_t{}; index != schema->field_count(); ++index) {
            const auto &field = schema->field(index);
            if (field.type == std::addressof(cpp_scalar_type<bool>()))
                columns.emplace_back(std::in_place_type<ArrowColumnReadSource<bool>>,
                                     table->column(static_cast<int>(index)));
            else if (field.type == std::addressof(cpp_scalar_type<i64>()))
                columns.emplace_back(std::in_place_type<ArrowColumnReadSource<i64>>,
                                     table->column(static_cast<int>(index)));
            else
                columns.emplace_back(std::in_place_type<ArrowColumnReadSource<double>>,
                                     table->column(static_cast<int>(index)));
        }
    }

    std::shared_ptr<arrow::Table> table;
    const bg::Schema *schema{};
    std::vector<ArrowRuntimeFrameColumn> columns;
    std::size_t size{};
};

template <cell_type T> void bind_runtime_arrow_column_view(const void *source, void *destination) {
    const auto &column = std::get<ArrowColumnReadSource<T>>(*static_cast<const ArrowRuntimeFrameColumn *>(source));
    *static_cast<ColumnView<T> *>(destination) = ColumnView<T>{column};
}

inline FrameColumnReadBinding runtime_arrow_frame_column_binding(const void *context, std::size_t index) {
    const auto &storage = *static_cast<const ArrowRuntimeFrameStorage *>(context);
    const auto &field = storage.schema->field(index);
    const auto *column = std::addressof(storage.columns.at(index));
    if (field.type == std::addressof(cpp_scalar_type<bool>()))
        return {std::addressof(cpp_scalar_type<bool>()), column, &bind_runtime_arrow_column_view<bool>, nullptr};
    if (field.type == std::addressof(cpp_scalar_type<i64>()))
        return {std::addressof(cpp_scalar_type<i64>()), column, &bind_runtime_arrow_column_view<i64>, nullptr};
    return {std::addressof(cpp_scalar_type<double>()), column, &bind_runtime_arrow_column_view<double>, nullptr};
}

inline FrameReadStorage frame_read_storage(const ArrowRuntimeFrameStorage &storage, const RuntimeType *runtime) {
    auto result = FrameReadStorage{};
    result.schema = storage.schema;
    result.valid = true;
    result.size = storage.size;
    result.columns_context = std::addressof(storage);
    result.column_binding = &runtime_arrow_frame_column_binding;
    result.source_owner = std::addressof(storage);
    result.source_runtime = runtime;
    return result;
}

inline arrow::Status validate_frame(const std::shared_ptr<arrow::Table> &table, const bg::Schema &schema) {
    if (!table)
        return arrow::Status::Invalid("Arrow table is null");
    if (!schema.is_resolved())
        return arrow::Status::Invalid("Arrow frame import requires a resolved schema");
    if (table->num_columns() != static_cast<int>(schema.field_count()) ||
        table->columns().size() != schema.field_count())
        return arrow::Status::Invalid("Arrow table column count does not match the frame schema");

    for (auto index = std::size_t{}; index != schema.field_count(); ++index) {
        const auto &field = schema.field(index);
        if (field.optional)
            return arrow::Status::Invalid("Arrow frame schema fields must be present");
        if (table->field(static_cast<int>(index))->name() != field.name.text())
            return arrow::Status::Invalid("Arrow table column name does not match the frame schema");

        arrow::Status status;
        if (field.type == std::addressof(cpp_scalar_type<bool>())) {
            status = check_chunked_buffers<bool>(table->column(static_cast<int>(index)));
        } else if (field.type == std::addressof(cpp_scalar_type<i64>())) {
            status = check_chunked_buffers<i64>(table->column(static_cast<int>(index)));
        } else if (field.type == std::addressof(cpp_scalar_type<double>())) {
            status = check_chunked_buffers<double>(table->column(static_cast<int>(index)));
        } else {
            return arrow::Status::TypeError("frame schema has an unsupported cell type");
        }
        ARROW_RETURN_NOT_OK(status);
    }
    return table->ValidateFull();
}

} // namespace arrow_details

// The result borrows cell buffers and is valid only while the source view remains unchanged in its BG invocation.
template <cell_type T>
arrow::Result<std::shared_ptr<arrow::ChunkedArray>> borrow_arrow(const ColumnView<T> &column) {
    return arrow_details::export_column(column, false);
}

inline arrow::Result<std::shared_ptr<arrow::ChunkedArray>> borrow_arrow(const ColumnView<> &column) {
    return arrow_details::export_column(column, false);
}

// The result owns dense value and validity buffers, including initialized bytes for null cells.
template <cell_type T>
arrow::Result<std::shared_ptr<arrow::ChunkedArray>> copy_to_arrow(const ColumnView<T> &column) {
    return arrow_details::export_column(column, true);
}

inline arrow::Result<std::shared_ptr<arrow::ChunkedArray>> copy_to_arrow(const ColumnView<> &column) {
    return arrow_details::export_column(column, true);
}

template <template <template <typename, auto...> typename> typename Schema>
arrow::Result<std::shared_ptr<arrow::Table>> borrow_arrow(const FrameView<Schema> &frame) {
    return arrow_details::export_runtime_frame<false>(frame);
}

// Each copied frame column contains one dense, owned chunk, even when the source has multiple batches.
template <template <template <typename, auto...> typename> typename Schema>
arrow::Result<std::shared_ptr<arrow::Table>> copy_to_arrow(const FrameView<Schema> &frame) {
    return arrow_details::export_runtime_frame<true>(frame);
}

// Imports retain the supplied Arrow arrays. Their buffers must remain alive through those Arrow owners; importing
// a borrowed export does not extend the BG source lifetime. Use copy_to_arrow before retaining borrowed data.
template <cell_type T>
arrow::Result<ScalarValue<>> from_arrow(std::shared_ptr<arrow::ChunkedArray> column) {
    ARROW_RETURN_NOT_OK(arrow_details::validate_chunked<T>(column));
    auto storage = arrow_details::ArrowColumnReadSource<T>{std::move(column)};
    const auto &runtime = arrow_details::column_scalar_runtime<T>();
    auto value = Scalar<Column<T>>::from(runtime, std::addressof(storage));
    return ScalarValue<>{value};
}

inline arrow::Result<ScalarValue<>> from_arrow(std::shared_ptr<arrow::Table> table, const bg::Schema &schema) {
    ARROW_RETURN_NOT_OK(arrow_details::validate_frame(table, schema));
    auto storage = arrow_details::ArrowRuntimeFrameStorage{std::move(table), schema};
    try {
        details::validate_frame_key_uniqueness(arrow_details::frame_read_storage(storage, nullptr));
    } catch (const std::invalid_argument &error) {
        return arrow::Status::Invalid(error.what());
    }
    const auto &type = frame_type(schema);
    const auto &runtime = arrow_details::runtime_schema_frame_scalar_runtime(type);
    auto value = Scalar<Frame<bg::SCHEMA>>::from(runtime, std::addressof(storage));
    return ScalarValue<>{value};
}

inline arrow::Result<ScalarValue<>> from_arrow(std::shared_ptr<arrow::Table> table) {
    if (!table)
        return arrow::Status::Invalid("Arrow table is null");

    auto fields = std::vector<SchemaField>{};
    fields.reserve(static_cast<std::size_t>(table->num_columns()));
    for (auto index = 0; index != table->num_columns(); ++index) {
        const auto field = table->field(index);
        for (auto prior = 0; prior != index; ++prior)
            if (table->field(prior)->name() == field->name())
                return arrow::Status::Invalid("Arrow table has duplicate column names");

        const ScalarType *cell_type{};
        if (field->type()->id() == arrow::Type::INT64)
            cell_type = std::addressof(cpp_scalar_type<i64>());
        else if (field->type()->id() == arrow::Type::BOOL)
            cell_type = std::addressof(cpp_scalar_type<bool>());
        else if (field->type()->id() == arrow::Type::DOUBLE)
            cell_type = std::addressof(cpp_scalar_type<double>());
        else
            return arrow::Status::TypeError("Arrow frame import supports only boolean, int64, and float64 fields");
        fields.push_back({util::symbol{field->name()}, cell_type, false});
    }

    try {
        return from_arrow(std::move(table), anonymous_schema(fields));
    } catch (const std::invalid_argument &error) {
        return arrow::Status::Invalid(error.what());
    }
}

template <template <template <typename, auto...> typename> typename Schema>
arrow::Result<ScalarValue<>> from_arrow(std::shared_ptr<arrow::Table> table) {
    const auto *schema = dynamic_cast<const bg::Schema *>(std::addressof(frame_schema_type<Schema>()));
    if (schema == nullptr)
        return arrow::Status::Invalid("Arrow frame import requires a resolved schema");
    return from_arrow(std::move(table), *schema);
}

} // namespace bg::frame
