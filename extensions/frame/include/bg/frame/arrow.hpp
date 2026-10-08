#pragma once

#include <bg/frame/frame.hpp>

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
#include <vector>

namespace bg::frame {

namespace arrow_details {

using scalar_runtime_factory = std::unique_ptr<ScalarRuntimeType> (*)();
const ScalarRuntimeType &canonical_scalar_runtime(const ScalarType &type, const std::type_info &identity,
                                                  scalar_runtime_factory factory);

template <cell_type T> std::shared_ptr<arrow::DataType> data_type() {
    if constexpr (std::same_as<T, i64>)
        return arrow::int64();
    else
        return arrow::float64();
}

template <cell_type T> constexpr auto arrow_type_id =
    std::same_as<T, i64> ? arrow::Type::INT64 : arrow::Type::DOUBLE;

inline arrow::Status check_length(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        return arrow::Status::CapacityError("frame value exceeds Arrow's signed 64-bit length limit");
    return arrow::Status::OK();
}

template <cell_type T> arrow::Status check_array_buffers(const std::shared_ptr<arrow::Array> &array) {
    if (!array || array->type_id() != arrow_type_id<T>)
        return arrow::Status::TypeError("Arrow column has an unsupported physical type");
    const auto &buffers = array->data()->buffers;
    if (buffers.size() < 2)
        return arrow::Status::Invalid("Arrow numeric array has fewer than two buffers");
    for (const auto &buffer : buffers)
        if (buffer && !buffer->is_cpu())
            return arrow::Status::NotImplemented("Arrow numeric buffers must be on the CPU");
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
        const auto *values = chunk->data()->buffers[1]->data_as<T>();
        return values[value_position];
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
            const auto *values = buffers[1] == nullptr
                                     ? nullptr
                                     : buffers[1]->data_as<T>() + static_cast<std::size_t>(offset);
            std::invoke(visit, target,
                        ColumnBatch<T>{offset, length, values, validity});
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
    if (batch.size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / sizeof(T)))
        return arrow::Status::CapacityError("Arrow value buffer exceeds the signed 64-bit size limit");

    auto validity_bytes = std::vector<std::uint8_t>(batch.size / 8 + (batch.size % 8 != 0), 0);
    auto null_count = std::int64_t{};
    for (auto position = std::size_t{}; position != batch.size; ++position) {
        if (batch.is_valid(position)) {
            arrow::bit_util::SetBit(validity_bytes.data(), static_cast<std::int64_t>(position));
        } else {
            ++null_count;
        }
    }

    std::shared_ptr<arrow::Buffer> validity;
    if (null_count != 0)
        validity = arrow::Buffer::FromVector(std::move(validity_bytes));

    std::shared_ptr<arrow::Buffer> values;
    const auto byte_size = static_cast<std::int64_t>(batch.size * sizeof(T));
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
                                       {std::move(validity), std::move(values)}, null_count);
    return arrow::MakeArray(data);
}

template <cell_type T>
arrow::Result<std::shared_ptr<arrow::ChunkedArray>> export_column(const ColumnView<T> &column, bool copy_values) {
    ARROW_RETURN_NOT_OK(check_length(column.size()));
    if (copy_values) {
        if (column.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / sizeof(T)))
            return arrow::Status::CapacityError("Arrow value buffer exceeds the signed 64-bit size limit");
        auto dense_values = std::vector<T>(column.size(), T{});
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
                if (batch.values == nullptr) {
                    status = arrow::Status::Invalid("column batch has valid cells without a value buffer");
                    return;
                }
                dense_values[position] = batch.values[offset];
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

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
std::shared_ptr<arrow::Schema> make_schema(std::index_sequence<Index...>) {
    constexpr auto names = util::aggregate_member_names<Schema<Column>>;
    auto fields = std::vector<std::shared_ptr<arrow::Field>>{};
    fields.reserve(sizeof...(Index));
    (fields.push_back(arrow::field(std::string{names[Index]},
                                   data_type<frame::details::frame_schema_cell_t<Index, Schema>>(), true)),
     ...);
    return arrow::schema(std::move(fields));
}

template <template <template <typename, auto...> typename> typename Schema, bool CopyValues,
          std::size_t... Index>
arrow::Result<std::shared_ptr<arrow::Table>> export_frame(const FrameView<Schema> &frame,
                                                          std::index_sequence<Index...>) {
    ARROW_RETURN_NOT_OK(check_length(frame.size()));
    auto columns = std::vector<std::shared_ptr<arrow::ChunkedArray>>{};
    columns.reserve(sizeof...(Index));
    auto source = util::aggregate_members(frame.columns);
    auto status = arrow::Status::OK();
    (([&] {
         if (!status.ok())
             return;
         auto column = export_column(std::get<Index>(source), CopyValues);
         if (!column.ok()) {
             status = column.status();
             return;
         }
         if (column.ValueUnsafe()->length() != static_cast<std::int64_t>(frame.size())) {
             status = arrow::Status::Invalid("frame column length does not match the frame row count");
             return;
         }
         columns.push_back(std::move(column).ValueUnsafe());
     }()),
     ...);
    ARROW_RETURN_NOT_OK(status);
    return arrow::Table::Make(make_schema<Schema>(std::index_sequence<Index...>{}), std::move(columns),
                              static_cast<std::int64_t>(frame.size()));
}

template <typename Semantic, typename Storage, typename Value>
class ArrowScalarRuntime final : public ScalarRuntimeType {
  public:
    ArrowScalarRuntime()
        : ScalarRuntimeType{scalar_type_traits<Semantic>::type(), sizeof(Storage), alignof(Storage), false, true,
                            false} {}

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
    bool equals(const void *, const ScalarData &) const noexcept override { return false; }
    bool equals_same_type(const void *, const ScalarData &) const noexcept override { return false; }
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
    return scalar_runtime<Column<T>, ArrowColumnReadSource<T>, ColumnView<T>>();
}

template <template <template <typename, auto...> typename> typename Schema> struct ArrowFrameStorage {
    using source_schema = Schema<ArrowColumnReadSource>;

    ArrowFrameStorage() = default;
    explicit ArrowFrameStorage(std::shared_ptr<arrow::Table> input) : table{std::move(input)} {
        auto fields = util::aggregate_members(sources);
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            ((std::get<Index>(fields).bind(table->column(static_cast<int>(Index)))), ...);
        }(std::make_index_sequence<util::aggregate_member_count<Schema<Column>>>{});
        size = static_cast<std::size_t>(table->num_rows());
    }

    std::shared_ptr<arrow::Table> table;
    source_schema sources;
    std::size_t size{};
};

template <template <template <typename, auto...> typename> typename Schema>
const ArrowScalarRuntime<Frame<Schema>, ArrowFrameStorage<Schema>, FrameView<Schema>> &frame_scalar_runtime();

template <cell_type T> void bind_arrow_column_view(const void *source, void *destination) {
    *static_cast<ColumnView<T> *>(destination) =
        ColumnView<T>{*static_cast<const ArrowColumnReadSource<T> *>(source)};
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t Index>
FrameColumnReadBinding arrow_frame_column_binding(const void *context) {
    using Cell = frame::details::frame_schema_cell_t<Index, Schema>;
    const auto fields = util::aggregate_members(
        *static_cast<const typename ArrowFrameStorage<Schema>::source_schema *>(context));
    return {std::addressof(cpp_scalar_type<Cell>()), std::addressof(std::get<Index>(fields)),
            &bind_arrow_column_view<Cell>, nullptr};
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
FrameColumnReadBinding arrow_frame_column_binding(const void *context, std::size_t index,
                                                   std::index_sequence<Index...>) {
    using binding_function = FrameColumnReadBinding (*)(const void *);
    static constexpr std::array<binding_function, sizeof...(Index)> bindings{
        &arrow_frame_column_binding<Schema, Index>...};
    return index < bindings.size() ? bindings[index](context) : FrameColumnReadBinding{};
}

template <template <template <typename, auto...> typename> typename Schema>
FrameReadStorage frame_read_storage(const ArrowFrameStorage<Schema> &storage) {
    constexpr auto count = util::aggregate_member_count<Schema<Column>>;
    static_assert(!bg::details::schema_variable_template<Schema>);
    auto result = FrameReadStorage{};
    result.schema = std::addressof(bg::details::cpp_hierarchical_scalar_schema<Schema>());
    result.valid = true;
    result.size = storage.size;
    result.columns_context = std::addressof(storage.sources);
    result.column_binding = [](const void *context, std::size_t index) {
        return arrow_frame_column_binding<Schema>(context, index, std::make_index_sequence<count>{});
    };
    result.source_owner = std::addressof(storage);
    result.source_payload = std::addressof(storage);
    result.source_runtime = std::addressof(frame_scalar_runtime<Schema>());
    return result;
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
FrameView<Schema> frame_view(const ArrowFrameStorage<Schema> &storage, std::index_sequence<Index...>) {
    auto columns = Schema<ColumnView>{};
    auto destination = util::aggregate_members(columns);
    auto source = util::aggregate_members(storage.sources);
    ((std::get<Index>(destination) = ColumnView<frame::details::frame_schema_cell_t<Index, Schema>>{
          std::get<Index>(source)}),
     ...);
    return FrameView<Schema>{std::move(columns), storage.size};
}

template <template <template <typename, auto...> typename> typename Schema>
struct ArrowScalarValueRead<ArrowFrameStorage<Schema>, FrameView<Schema>> {
    static FrameView<Schema> read(const ArrowFrameStorage<Schema> &source) {
        return frame_view(source, std::make_index_sequence<util::aggregate_member_count<Schema<Column>>>{});
    }
};

template <template <template <typename, auto...> typename> typename Schema>
const ArrowScalarRuntime<Frame<Schema>, ArrowFrameStorage<Schema>, FrameView<Schema>> &frame_scalar_runtime() {
    return scalar_runtime<Frame<Schema>, ArrowFrameStorage<Schema>, FrameView<Schema>>();
}

template <template <template <typename, auto...> typename> typename Schema>
arrow::Status validate_frame(const std::shared_ptr<arrow::Table> &table) {
    if (!table)
        return arrow::Status::Invalid("Arrow table is null");
    constexpr auto count = util::aggregate_member_count<Schema<Column>>;
    if (table->num_columns() != static_cast<int>(count))
        return arrow::Status::Invalid("Arrow table column count does not match the frame schema");
    if (table->columns().size() != count)
        return arrow::Status::Invalid("Arrow table data column count does not match its schema");
    constexpr auto names = util::aggregate_member_names<Schema<Column>>;
    auto status = arrow::Status::OK();
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        (([&] {
             if (!status.ok())
                 return;
             const auto field = table->field(static_cast<int>(Index));
             if (field->name() != names[Index]) {
                 status = arrow::Status::Invalid("Arrow table column name does not match the frame schema");
                 return;
             }
             status = check_chunked_buffers<frame::details::frame_schema_cell_t<Index, Schema>>(
                 table->column(static_cast<int>(Index)));
         }()),
         ...);
    }(std::make_index_sequence<count>{});
    if (status.ok())
        status = table->ValidateFull();
    return status;
}

} // namespace arrow_details

// The result borrows cell buffers and is valid only while the source view remains unchanged in its BG invocation.
template <cell_type T>
arrow::Result<std::shared_ptr<arrow::ChunkedArray>> borrow_arrow(const ColumnView<T> &column) {
    return arrow_details::export_column(column, false);
}

// The result owns dense value and validity buffers, including initialized bytes for null cells.
template <cell_type T>
arrow::Result<std::shared_ptr<arrow::ChunkedArray>> copy_to_arrow(const ColumnView<T> &column) {
    return arrow_details::export_column(column, true);
}

template <template <template <typename, auto...> typename> typename Schema>
arrow::Result<std::shared_ptr<arrow::Table>> borrow_arrow(const FrameView<Schema> &frame) {
    return arrow_details::export_frame<Schema, false>(frame,
                                                      std::make_index_sequence<util::aggregate_member_count<
                                                          Schema<Column>>>{});
}

// Each copied frame column contains one dense, owned chunk, even when the source has multiple batches.
template <template <template <typename, auto...> typename> typename Schema>
arrow::Result<std::shared_ptr<arrow::Table>> copy_to_arrow(const FrameView<Schema> &frame) {
    return arrow_details::export_frame<Schema, true>(frame,
                                                     std::make_index_sequence<util::aggregate_member_count<
                                                         Schema<Column>>>{});
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

template <template <template <typename, auto...> typename> typename Schema>
arrow::Result<ScalarValue<>> from_arrow(std::shared_ptr<arrow::Table> table) {
    ARROW_RETURN_NOT_OK(arrow_details::validate_frame<Schema>(table));
    auto storage = arrow_details::ArrowFrameStorage<Schema>{std::move(table)};
    const auto &runtime = arrow_details::frame_scalar_runtime<Schema>();
    auto value = Scalar<Frame<Schema>>::from(runtime, std::addressof(storage));
    return ScalarValue<>{value};
}

} // namespace bg::frame
