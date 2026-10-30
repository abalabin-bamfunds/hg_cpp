#include <bg/frame/frame.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/frame/literal.hpp>
#include "format.hpp"
#include <bg/type/type_registry.hpp>
#include <bg/util/scope_exit.hpp>
#include "tick_codec.hpp"
#include "queue_operations.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <variant>

namespace bg::frame {
namespace {

const ScalarRuntimeType &default_frame_scalar_runtime_type(const ScalarType &type) {
    return frame_snapshot_runtime_type(static_cast<const FrameType &>(type));
}

const TimeSeriesRuntimeType &default_frame_series_runtime_type(const TimeSeriesType &type) {
    return details::canonical_frame_series_runtime_type(
        type, [](const TimeSeriesType &series_type) -> std::unique_ptr<TimeSeriesRuntimeType> {
            return std::make_unique<details::NativeFrameSeriesRuntimeType>(series_type);
        });
}

struct FrameSeriesKey {
    const SchemaType *schema;
    std::variant<TimeSeriesGuarantee, const GuaranteeVariable *> guarantee;

    bool operator==(const FrameSeriesKey &) const noexcept = default;
};

struct FrameSeriesKeyHash {
    std::size_t operator()(const FrameSeriesKey &key) const noexcept {
        auto result = std::hash<const SchemaType *>{}(key.schema);
        const auto guarantee_hash = std::visit(
            [](const auto &value) -> std::size_t {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, TimeSeriesGuarantee>)
                    return std::hash<std::underlying_type_t<TimeSeriesGuarantee>>{}(std::to_underlying(value));
                else
                    return std::hash<const GuaranteeVariable *>{}(value);
            },
            key.guarantee);
        return result ^ (guarantee_hash + 0x9e3779b9 + (result << 6) + (result >> 2));
    }
};

FrameSeriesKey frame_series_key(const SchemaType &schema, const GuaranteeParameter &guarantee) {
    if (const auto value = guarantee.value()) {
        if (*value != TimeSeriesGuarantee::ordinary)
            throw std::invalid_argument{"TSF requires ordinary"};
        return {std::addressof(schema), *value};
    }
    if (const auto *variable = guarantee.variable())
        return {std::addressof(schema), variable};
    throw std::invalid_argument{"a frame time series requires a guarantee"};
}

constexpr auto frame_key_metadata_name = std::string_view{"bg.frame.key"};

std::vector<std::size_t> frame_key_fields(const bg::Schema &schema) {
    const auto *metadata = schema.metadata().find(frame_key_metadata_name);
    if (metadata == nullptr)
        return {};
    if (std::addressof(metadata->type()) != std::addressof(cpp_scalar_type<bg::List<bg::string>>()))
        throw std::invalid_argument{"frame key metadata must be a List<string>"};

    const auto names = bg::Scalar<bg::List<bg::string>>::from(metadata->runtime_type(), metadata->data());
    if (names.empty())
        throw std::invalid_argument{"frame key metadata must name at least one column"};

    auto result = std::vector<std::size_t>{};
    result.reserve(names.size());
    for (auto index = std::size_t{}; index != names.size(); ++index) {
        const auto name = names[index].value();
        if (name.empty())
            throw std::invalid_argument{"frame key metadata contains an empty column name"};
        const auto *field = schema.find(util::symbol{name});
        if (field == nullptr)
            throw std::invalid_argument{"frame key metadata names a missing column"};
        if (field->type->is_resolved() && field->type != std::addressof(cpp_scalar_type<bg::i64>()) &&
            field->type != std::addressof(cpp_scalar_type<string>()) &&
            field->type != std::addressof(cpp_scalar_type<symbol>()))
            throw std::invalid_argument{"frame key columns must have i64, string, or symbol cell types"};

        const auto field_index = static_cast<std::size_t>(field - schema.fields().data());
        if (std::ranges::find(result, field_index) != result.end())
            throw std::invalid_argument{"frame key metadata names a column more than once"};
        result.push_back(field_index);
    }
    return result;
}

struct FrameRuntimeCache {
    std::mutex series_mutex;
    std::mutex layout_mutex;
    std::unordered_map<const TimeSeriesType *, std::unique_ptr<TimeSeriesRuntimeType>> series_runtimes;
    std::unordered_map<const FrameSeriesRuntimeType *, std::unique_ptr<ScalarRuntimeType>> series_scalar_runtimes;
    std::unordered_map<const ScalarType *, std::unique_ptr<ScalarRuntimeType>> snapshot_runtimes;
    std::unordered_map<const SchemaType *, std::unique_ptr<details::NativeFrameLayout>> layouts;
};

FrameRuntimeCache &frame_runtime_cache() {
    static util::static_storage<FrameRuntimeCache> cache;
    return cache.get();
}

void validate_frame_schema(const SchemaType &schema) {
    const auto *concrete = dynamic_cast<const Schema *>(std::addressof(schema));
    if (concrete == nullptr)
        return;
    for (auto current = concrete; current != nullptr; current = current->base()) {
        static_cast<void>(frame_key_fields(*current));
        if (current->base() == nullptr)
            continue;

        const auto *base_key = current->base()->metadata().find(frame_key_metadata_name);
        const auto *direct_key = current->direct_metadata().find(frame_key_metadata_name);
        if (base_key != nullptr && direct_key != nullptr && !base_key->equals(*direct_key))
            throw std::invalid_argument{"a derived frame schema cannot change its inherited key"};
    }
    for (const auto &field : concrete->fields()) {
        if (field.optional)
            throw std::invalid_argument{"frame schema columns must be present"};
        if (type_cast<ScalarType>(field.type) == nullptr)
            throw std::invalid_argument{"frame schema fields must have scalar types"};
    }
}

bool frame_schema_matches(const SchemaType &parameter, const SchemaType &argument, TypeMap &type_map) {
    auto trial = type_map.overlay();
    const auto *parameter_schema = dynamic_cast<const Schema *>(std::addressof(parameter));
    const auto *argument_schema = dynamic_cast<const Schema *>(std::addressof(argument));
    if (parameter_schema != nullptr && argument_schema != nullptr) {
        const auto *parameter_key = parameter_schema->metadata().find(frame_key_metadata_name);
        const auto *argument_key = argument_schema->metadata().find(frame_key_metadata_name);
        if (parameter_key != nullptr && (argument_key == nullptr || !parameter_key->equals(*argument_key)))
            return false;

        if (parameter_schema->named() && argument_schema->named() &&
            !argument_schema->derives_from(*parameter_schema))
            return false;

        for (const auto &field : parameter_schema->fields()) {
            const auto *argument_field = argument_schema->find(field.name);
            if (argument_field == nullptr || !field.type->match(*argument_field->type, trial))
                return false;
        }
    } else if (!parameter.match(argument, trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

using FrameKeyCell = std::variant<std::monostate, i64, std::string_view, symbol>;

template <cell_type T> struct KeyColumnBatches {
    std::vector<typename ColumnView<T>::Batch> batches;
    std::vector<std::size_t> starts;
    std::size_t next_batch{};

    FrameKeyCell next_cell(std::size_t position) {
        while (position >= starts[next_batch] + batches[next_batch].size)
            ++next_batch;
        return cell(next_batch, position - starts[next_batch]);
    }

    FrameKeyCell cell_at(std::size_t position) const {
        const auto next = std::upper_bound(starts.begin(), starts.end(), position);
        const auto index = static_cast<std::size_t>(next - starts.begin() - 1);
        return cell(index, position - starts[index]);
    }

  private:
    FrameKeyCell cell(std::size_t batch_index, std::size_t offset) const {
        const auto &batch = batches[batch_index];
        return batch.is_valid(offset) ? FrameKeyCell{batch.value_at(offset)} : FrameKeyCell{std::monostate{}};
    }
};

using ErasedKeyColumnBatches =
    std::variant<KeyColumnBatches<i64>, KeyColumnBatches<string>, KeyColumnBatches<symbol>>;

template <cell_type T> KeyColumnBatches<T> key_column_batches(const FrameReadStorage &source, std::size_t index) {
    const auto binding = source.column(index);
    auto view = ColumnView<T>{};
    binding.bind_column_view(binding.payload, std::addressof(view));
    auto result = KeyColumnBatches<T>{};
    auto position = std::size_t{};
    view.batches([&](const ColumnView<T>::Batch &batch) {
        if (batch.size == 0)
            return;
        if (position > source.size || batch.size > source.size - position)
            throw std::invalid_argument{"frame key column batches exceed the frame row count"};
        result.starts.push_back(position);
        result.batches.push_back(batch);
        position += batch.size;
    });
    if (position != source.size)
        throw std::invalid_argument{"frame key column batches do not match the frame row count"};
    return result;
}

ErasedKeyColumnBatches key_column_batches(const FrameReadStorage &source, std::size_t index) {
    const auto binding = source.column(index);
    return visit_cell_type(*binding.cell_type, [&]<typename Cell>(std::type_identity<Cell>) -> ErasedKeyColumnBatches {
        if constexpr (std::same_as<Cell, i64> || std::same_as<Cell, string> || std::same_as<Cell, symbol>)
            return ErasedKeyColumnBatches{key_column_batches<Cell>(source, index)};
        else
            throw std::invalid_argument{"frame key has an unsupported cell type"};
    });
}

std::size_t frame_key_cell_hash(const FrameKeyCell &cell) {
    return std::visit(
        [](const auto &value) -> std::size_t {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Value, std::monostate>)
                return details::null_key_hash;
            else if constexpr (std::same_as<Value, std::string_view>)
                return scalar_storage_traits<string>::hash(value);
            else
                return scalar_storage_traits<Value>::hash(value);
        },
        cell);
}

std::size_t combine_frame_key_hash(std::size_t result, std::size_t value) noexcept {
    constexpr auto constant = std::size_t{0x9e3779b9};
    return result ^ (value + constant + (result << 6) + (result >> 2));
}

class FrameCopySource {
  public:
    using Cell = std::variant<std::optional<bool>, std::optional<i64>, std::optional<double>,
                              std::optional<std::string_view>, std::optional<symbol>>;
    using Column = std::variant<ColumnSeriesView<bool>, ColumnSeriesView<i64>, ColumnSeriesView<double>,
                                ColumnSeriesView<string>, ColumnSeriesView<symbol>>;

    FrameCopySource(const FrameReadStorage &input, const details::NativeFrameLayout &layout)
        : input_{input}, cells_(layout.fields.size()), values_(layout.fields.size()), patches_(layout.fields.size()) {
        if (input.schema != layout.schema) {
            if (!input.schema->derives_from(*layout.schema))
                throw std::invalid_argument{"frame copy requires the same schema or a derived schema"};

            const auto source_fields = input.schema->fields();
            const auto destination_fields = layout.schema->fields();
            for (auto index = std::size_t{}; index != destination_fields.size(); ++index) {
                if (source_fields[index].name != destination_fields[index].name ||
                    source_fields[index].type != destination_fields[index].type)
                    throw std::invalid_argument{"frame copy requires matching inherited field types"};
            }
        }

        columns_.reserve(layout.fields.size());
        for (auto index = std::size_t{}; index != layout.fields.size(); ++index) {
            const auto binding = input.column(index);
            columns_.push_back(visit_cell_type(
                *layout.fields[index].cell_type, [&]<typename Cell>(std::type_identity<Cell>) {
                    auto column = std::optional<ColumnSeriesView<Cell>>{};
                    binding.bind_column_series_view(binding.payload, std::addressof(column));
                    return Column{*column};
                }));
        }
    }

    std::span<const void *const> row_values(row_id_t source_id) {
        for (auto index = std::size_t{}; index != columns_.size(); ++index) {
            cells_[index] = std::visit(
                [source_id](const auto &column) -> Cell { return column.cell(source_id); }, columns_[index]);
            values_[index] =
                std::visit([](auto &value) -> const void * { return std::addressof(value); }, cells_[index]);
        }
        return values_;
    }

    std::span<const details::NativeFrameCellPatch> row_patches(row_id_t source_id) {
        for (auto index = std::size_t{}; index != columns_.size(); ++index) {
            const auto *modified = input_.modified_at(index);
            if (modified == nullptr || !modified->contains(source_id)) {
                patches_[index] = {};
                continue;
            }
            cells_[index] = std::visit(
                [source_id](const auto &column) -> Cell { return column.cell(source_id); }, columns_[index]);
            const auto *value =
                std::visit([](auto &cell) -> const void * { return std::addressof(cell); }, cells_[index]);
            patches_[index] = {true, value};
        }
        return patches_;
    }

  private:
    const FrameReadStorage &input_;
    std::vector<Column> columns_;
    std::vector<Cell> cells_;
    std::vector<const void *> values_;
    std::vector<details::NativeFrameCellPatch> patches_;
};

} // namespace

bool NativeFrameStorage::copy_from(const FrameReadStorage &input, EvaluationTime time, allocator memory,
                                   bool full_copy) {
    if (!input.valid)
        throw std::invalid_argument{"cannot copy an invalid frame input"};
    if (!full_copy && input.change_time != input.last_updated)
        return false;
    auto source = FrameCopySource{input, *layout_};
    if (full_copy) {
        clear(time, memory);
        publish_all_columns(time, memory);
        copy_source_domain_ = input.rows.identity;
        input.rows.each([&](row_id_t source_id) {
            const auto destination_id = add(source.row_values(source_id), time, memory);
            map_source_row(source_id, destination_id, memory);
        });
        return true;
    }

    reserve_changed_columns(memory);
    begin_cycle(time, memory);

    if (input.removed != nullptr) {
        for (auto source_id : *input.removed) {
            const auto *destination_id = copied_rows_.find(source_id);
            if (destination_id == nullptr)
                continue;
            const auto output_id = *destination_id;
            copied_rows_.erase(source_id, memory.memory_resource());
            erase(output_id, time, memory);
        }
    }
    if (input.added != nullptr) {
        for (auto source_id : *input.added) {
            if (!input.rows.has(source_id) || copied_rows_.contains(source_id))
                continue;
            const auto destination_id = add(source.row_values(source_id), time, memory);
            map_source_row(source_id, destination_id, memory);
        }
    }
    if (input.changed != nullptr) {
        for (auto source_id : *input.changed) {
            if (!input.rows.has(source_id) || (input.added != nullptr && input.added->contains(source_id)))
                continue;
            const auto *destination_id = copied_rows_.find(source_id);
            if (destination_id == nullptr)
                throw std::logic_error{"frame copy row mapping is missing; a full copy is required"};
            const auto patches = source.row_patches(source_id);
            static_cast<void>(set(*destination_id, patches, time, memory));
        }
    }

    for (auto index = std::size_t{}; index != layout_->fields.size(); ++index) {
        if (!input.has_changed_column(index))
            continue;
        publish_column(index, time, memory);
    }
    return true;
}

void NativeFrameStorage::replay_copy_tick(util::buffer_reader &reader, EvaluationTime time, allocator memory) {
    const auto &schema = details::tick_codec::read_schema(reader);
    if (std::addressof(schema) != layout_->schema)
        throw std::invalid_argument{"frame tick replay requires a matching schema"};
    const auto source_domain = reinterpret_cast<const void *>(reader.read_raw<std::uintptr_t>());
    const auto changed_columns = details::tick_codec::read_changed_columns(reader);

    if (copy_source_domain_ != source_domain) {
        clear(time, memory);
        copy_source_domain_ = source_domain;
    } else {
        reserve_changed_columns(memory);
        begin_cycle(time, memory);
    }

    const auto cell_value = [](const details::tick_codec::Cell &cell) -> const void * {
        return std::visit(
            [](const auto &value) -> const void * {
                using Cell = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Cell, std::monostate>)
                    throw std::invalid_argument{"frame add tick is missing a cell value"};
                else
                    return std::addressof(value);
            },
            cell);
    };

    for (;;) {
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation == bg::details::TickOperation::end)
            break;
        if (header.operation != bg::details::TickOperation::add &&
            header.operation != bg::details::TickOperation::tick &&
            header.operation != bg::details::TickOperation::remove)
            throw std::invalid_argument{"invalid frame tick operation"};

        auto change = details::tick_codec::read_change(reader, *layout_->schema, *layout_, header);
        if (header.operation == bg::details::TickOperation::remove) {
            const auto *destination_id = copied_rows_.find(change.id);
            if (destination_id == nullptr)
                continue;
            const auto output_id = *destination_id;
            copied_rows_.erase(change.id, memory.memory_resource());
            erase(output_id, time, memory);
            continue;
        }

        if (header.operation == bg::details::TickOperation::add) {
            if (copied_rows_.contains(change.id))
                throw std::logic_error{"frame tick added an active row ID"};
            auto values = std::vector<const void *>{};
            values.reserve(change.cells.size());
            for (const auto &cell : change.cells)
                values.push_back(cell_value(cell));
            const auto destination_id = add(values, time, memory);
            map_source_row(change.id, destination_id, memory);
            continue;
        }

        const auto *destination_id = copied_rows_.find(change.id);
        if (destination_id == nullptr)
            throw std::logic_error{"frame tick row mapping is missing; a full copy is required"};
        auto patches = std::vector<details::NativeFrameCellPatch>(change.cells.size());
        for (auto index = std::size_t{}; index != change.cells.size(); ++index) {
            if (std::holds_alternative<std::monostate>(change.cells[index]))
                continue;
            patches[index] = {true, cell_value(change.cells[index])};
        }
        static_cast<void>(set(*destination_id, patches, time, memory));
    }

    for (const auto index : changed_columns)
        publish_column(index, time, memory);
    valid_ = true;
    bind_column_storage();
}

const details::NativeFrameLayout &details::native_frame_layout(const SchemaType &schema) {
    auto &cache = frame_runtime_cache();
    const auto lock = std::lock_guard{cache.layout_mutex};
    if (const auto found = cache.layouts.find(std::addressof(schema)); found != cache.layouts.end())
        return *found->second;

    validate_frame_schema(schema);
    const auto *concrete = dynamic_cast<const bg::Schema *>(std::addressof(schema));
    if (concrete == nullptr || !concrete->is_resolved())
        throw std::invalid_argument{"TSF native storage requires a resolved BG schema"};

    auto layout = std::make_unique<NativeFrameLayout>();
    layout->schema = concrete;
    layout->alignment = alignof(NativeFrameStorage);
    auto offset = sizeof(NativeFrameStorage);
    layout->key_fields = frame_key_fields(*concrete);
    for (const auto &schema_field : concrete->fields()) {
        if (schema_field.optional)
            throw std::invalid_argument{"frame schema columns must be present"};
        const auto *cell_type = type_cast<ScalarType>(schema_field.type);
        if (cell_type == nullptr)
            throw std::invalid_argument{"frame schema fields must have scalar types"};
        auto field = native_frame_field_layout(*cell_type);
        const auto align = [&](std::size_t alignment) {
            layout->alignment = std::max(layout->alignment, alignment);
            offset = (offset + alignment - 1) / alignment * alignment;
        };
        align(field.native_alignment);
        field.native_offset = offset;
        offset += field.native_size;
        align(field.child_state_alignment);
        field.child_state_offset = offset;
        offset += field.child_state_size;
        align(field.column_storage_alignment);
        field.column_storage_offset = offset;
        offset += field.column_storage_size;
        layout->fields.push_back(field);
    }
    if (!layout->key_fields.empty()) {
        const auto alignment = alignof(NativeFrameKeyState);
        layout->alignment = std::max(layout->alignment, alignment);
        offset = (offset + alignment - 1) / alignment * alignment;
        layout->key_state_offset = offset;
        offset += sizeof(NativeFrameKeyState);
    }
    layout->size = (offset + layout->alignment - 1) / layout->alignment * layout->alignment;
    const auto *result = layout.get();
    cache.layouts.emplace(std::addressof(schema), std::move(layout));
    return *result;
}

void details::validate_frame_key_uniqueness(const FrameReadStorage &source) {
    const auto &layout = native_frame_layout(*source.schema);
    if (layout.key_fields.empty() || source.size == 0)
        return;

    auto columns = std::vector<ErasedKeyColumnBatches>{};
    columns.reserve(layout.key_fields.size());
    for (auto index : layout.key_fields)
        columns.push_back(key_column_batches(source, index));

    constexpr auto no_position = std::numeric_limits<std::size_t>::max();
    auto next_rows = std::vector<std::size_t>(source.size, no_position);
    auto rows_by_hash = std::unordered_map<std::size_t, std::size_t>{};
    rows_by_hash.reserve(source.size);
    auto key = std::vector<FrameKeyCell>(columns.size());

    for (auto position = std::size_t{}; position != source.size; ++position) {
        auto hash = std::size_t{};
        for (auto index = std::size_t{}; index != columns.size(); ++index) {
            key[index] = std::visit([&](auto &column) { return column.next_cell(position); }, columns[index]);
            hash = combine_frame_key_hash(hash, frame_key_cell_hash(key[index]));
        }

        const auto [entry, inserted] = rows_by_hash.try_emplace(hash, position);
        if (inserted)
            continue;

        for (auto previous = entry->second; previous != no_position; previous = next_rows[previous]) {
            auto equal = true;
            for (auto index = std::size_t{}; index != columns.size(); ++index) {
                if (std::visit([&](const auto &column) { return column.cell_at(previous); }, columns[index]) !=
                    key[index]) {
                    equal = false;
                    break;
                }
            }
            if (equal)
                throw std::invalid_argument{"frame key is not unique"};
        }

        next_rows[position] = entry->second;
        entry->second = position;
    }
}

const FrameType &frame_type(const SchemaType &schema) {
    validate_frame_schema(schema);
    const auto &type = TypeRegistry::instance().get_or_create<FrameType, std::hash<const SchemaType *>>(
        std::addressof(schema), [&] { return std::make_unique<FrameType>(schema); });
    type.set_default_runtime_type_factory(&default_frame_scalar_runtime_type);
    details::register_frame_literal_type(type);
    details::register_frame_format_type(type);
    if (type.is_resolved()) {
        static_cast<void>(details::native_frame_layout(schema));
        static_cast<void>(frame_snapshot_runtime_type(type));
    }
    return type;
}

const FrameSeriesType &frame_series_type(const SchemaType &schema, GuaranteeParameter guarantee) {
    validate_frame_schema(schema);
    const auto key = frame_series_key(schema, guarantee);
    const auto &type = TypeRegistry::instance().get_or_create<FrameSeriesType, FrameSeriesKeyHash>(
        key, [&] { return std::make_unique<FrameSeriesType>(schema, std::move(guarantee)); });
    if (type.is_resolved()) {
        type.set_default_runtime_type_factory(&default_frame_series_runtime_type);
        const auto &runtime = details::canonical_frame_series_runtime_type(
            type, [](const TimeSeriesType &series_type) -> std::unique_ptr<TimeSeriesRuntimeType> {
                return std::make_unique<details::NativeFrameSeriesRuntimeType>(series_type);
            });
        register_time_series_runtime_type(runtime);
        details::register_frame_series_tick_comparison(type);
        details::register_frame_series_tick_replay(runtime);
        details::register_frame_series_queue_runtime(runtime);
        frame_series_scalar_runtime_type(static_cast<const FrameSeriesRuntimeType &>(runtime));
    }
    details::register_frame_tick_format_type(type);
    return type;
}

void FrameType::format_name(std::string &result) const {
    result += "Frame<";
    schema_type().format_name(result);
    result += '>';
}

FrameSeriesType::FrameSeriesType(const SchemaType &schema, GuaranteeParameter guarantee)
    : FixedItemTimeSeriesType{guarantee}, schema_{std::addressof(schema)} {
    const auto *concrete_schema = dynamic_cast<const Schema *>(std::addressof(schema));
    if (concrete_schema == nullptr)
        return;

    item_types_.reserve(concrete_schema->field_count());
    for (const auto &field : concrete_schema->fields()) {
        const auto *cell_type = type_cast<ScalarType>(field.type);
        if (cell_type == nullptr)
            throw std::invalid_argument{"frame schema fields must have scalar cell types"};
        item_types_.push_back(std::addressof(column_series_type(*cell_type, this->guarantee())));
    }
}

void FrameSeriesType::format_name(std::string &result) const {
    result += "TSF<";
    schema_type().format_name(result);
    append_guarantee(result);
    result += '>';
}

const Type &FrameType::resolve(const TypeMap &type_map) const {
    return frame_type(dynamic_cast<const SchemaType &>(schema_->resolve(type_map)));
}

bool FrameType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *frame = dynamic_cast<const FrameType *>(&supplied);
    if (frame == nullptr)
        return false;

    return frame_schema_matches(*schema_, frame->schema_type(), type_map);
}

const TimeSeriesType &FrameSeriesType::item_type(std::size_t index) const {
    return *item_types_.at(index);
}

const TimeSeriesType &FrameSeriesType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return frame_series_type(*schema_, guarantee);
}

const TimeSeriesType &FrameSeriesType::with_guarantee_limit(TimeSeriesGuarantee limit) const {
    return TimeSeriesType::with_guarantee_limit(limit);
}

const Type &FrameSeriesType::resolve(const TypeMap &type_map) const {
    return frame_series_type(dynamic_cast<const SchemaType &>(schema_->resolve(type_map)),
                             guarantee().resolve(type_map));
}

bool FrameSeriesType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *frame = dynamic_cast<const FrameSeriesType *>(&supplied);
    if (frame == nullptr || !frame->is_resolved())
        return false;

    auto trial = type_map.overlay();
    if (!frame_schema_matches(*schema_, frame->schema_type(), trial))
        return false;

    if (!guarantee().match(*frame->guarantee().value(), trial))
        return false;

    type_map.apply(std::move(trial));
    return true;
}

namespace details {

const TimeSeriesRuntimeType &canonical_frame_series_runtime_type(const TimeSeriesType &type,
                                                                 frame_series_runtime_factory factory) {
    auto &cache = frame_runtime_cache();
    const auto lock = std::lock_guard{cache.series_mutex};
    const auto *key = std::addressof(type);
    if (const auto entry = cache.series_runtimes.find(key); entry != cache.series_runtimes.end()) {
        if (&entry->second->type() != &type)
            throw std::logic_error{"frame runtime requested for a non-canonical schema type"};
        return *entry->second;
    }

    auto runtime = factory(type);
    const auto *result = runtime.get();
    const auto entry = cache.series_runtimes.emplace(key, std::move(runtime)).first;
    auto rollback = util::scope_exit{[&]() noexcept { cache.series_runtimes.erase(entry); }};
    register_frame_series_literal_runtime(*result);
    rollback.release();
    return *result;
}

} // namespace details

const ScalarRuntimeType &frame_series_scalar_runtime_type(const FrameSeriesRuntimeType &runtime) {
    const auto &schema = static_cast<const FrameSeriesType &>(runtime.type()).schema_type();
    const auto &snapshot_type = frame_type(schema);
    frame_snapshot_runtime_type(snapshot_type);

    if (const auto *cached = runtime.cached_scalar_runtime_type(); cached != nullptr)
        return *cached;
    auto &cache = frame_runtime_cache();
    const auto lock = std::lock_guard{cache.series_mutex};
    const auto *key = std::addressof(runtime);
    if (const auto found = cache.series_scalar_runtimes.find(key); found != cache.series_scalar_runtimes.end()) {
        runtime.cache_scalar_runtime_type(*found->second);
        return *found->second;
    }
    auto scalar_runtime = std::make_unique<details::FrameSeriesScalarRuntimeType>(runtime, snapshot_type);
    const auto *result = scalar_runtime.get();
    cache.series_scalar_runtimes.emplace(key, std::move(scalar_runtime));
    runtime.cache_scalar_runtime_type(*result);
    return *result;
}

const ScalarRuntimeType &frame_snapshot_runtime_type(const FrameType &type) {
    if (!type.is_resolved())
        throw std::invalid_argument{"frame snapshot runtime requires a resolved frame scalar type"};
    auto &cache = frame_runtime_cache();
    const auto lock = std::lock_guard{cache.series_mutex};
    const auto *key = std::addressof(type);
    if (const auto found = cache.snapshot_runtimes.find(key); found != cache.snapshot_runtimes.end())
        return *found->second;
    auto scalar_runtime = std::make_unique<details::FrameSnapshotRuntimeType>(type);
    const auto *result = scalar_runtime.get();
    const auto entry = cache.snapshot_runtimes.emplace(key, std::move(scalar_runtime)).first;
    auto rollback = util::scope_exit{[&]() noexcept { cache.snapshot_runtimes.erase(entry); }};
    register_scalar_runtime_type(*result);
    rollback.release();
    return *result;
}

const ScalarRuntimeType &FrameScalarRuntimeType::runtime_type_for_copy(const ScalarData &source) const {
    const auto storage = read_storage(source);
    if (storage.schema == nullptr)
        throw std::invalid_argument{"frame scalar source has no schema"};
    return frame_snapshot_runtime_type(frame_type(*storage.schema));
}

bool FrameScalarRuntimeType::equals(const void *value, const ScalarData &other) const {
    return equals(ScalarDataMode::stored_view, reinterpret_cast<std::uintptr_t>(value), other);
}

bool FrameScalarRuntimeType::equals(ScalarDataMode mode, std::uintptr_t data, const ScalarData &other) const {
    if (typeid(other.type()) != typeid(FrameType))
        return false;

    const auto left_data = ScalarData{*this, data, mode};
    const auto &right_runtime = static_cast<const FrameScalarRuntimeType &>(other.runtime_type());
    const auto left = read_storage(left_data);
    const auto right = right_runtime.read_storage(other);
    if (left.schema != right.schema || left.size != right.size)
        return false;

    for (auto index = std::size_t{}; index != left.schema->field_count(); ++index) {
        if (!column_values_equal(left.generic_column(index), right.generic_column(index)))
            return false;
    }
    return true;
}

bool FrameScalarRuntimeType::equals_same_type(const void *value, const ScalarData &other) const {
    return equals(ScalarDataMode::stored_view, reinterpret_cast<std::uintptr_t>(value), other);
}

} // namespace bg::frame
