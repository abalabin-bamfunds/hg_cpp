#include <bg/frame/frame.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/frame/literal.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <variant>

namespace bg::frame {
namespace {

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
        if (field->type->is_resolved() && field->type != std::addressof(cpp_scalar_type<bg::i64>()))
            throw std::invalid_argument{"frame key columns must have i64 cell types"};

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

struct KeyColumnBatches {
    std::vector<ColumnView<i64>::Batch> batches;
    std::vector<std::size_t> starts;
    std::size_t next_batch{};

    std::optional<i64> next_cell(std::size_t position) {
        while (position >= starts[next_batch] + batches[next_batch].size)
            ++next_batch;
        return cell(next_batch, position - starts[next_batch]);
    }

    std::optional<i64> cell_at(std::size_t position) const {
        const auto next = std::upper_bound(starts.begin(), starts.end(), position);
        const auto index = static_cast<std::size_t>(next - starts.begin() - 1);
        return cell(index, position - starts[index]);
    }

  private:
    std::optional<i64> cell(std::size_t batch_index, std::size_t offset) const {
        const auto &batch = batches[batch_index];
        return batch.is_valid(offset) ? std::optional<i64>{batch.values[offset]} : std::nullopt;
    }
};

KeyColumnBatches key_column_batches(const FrameReadStorage &source, std::size_t index) {
    const auto binding = source.column(index);
    auto view = ColumnView<i64>{};
    binding.bind_column_view(binding.payload, std::addressof(view));
    auto result = KeyColumnBatches{};
    auto position = std::size_t{};
    view.batches([&](const ColumnView<i64>::Batch &batch) {
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

std::size_t combine_frame_key_hash(std::size_t result, std::size_t value) noexcept {
    constexpr auto constant = std::size_t{0x9e3779b9};
    return result ^ (value + constant + (result << 6) + (result >> 2));
}

} // namespace

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

    auto columns = std::vector<KeyColumnBatches>{};
    columns.reserve(layout.key_fields.size());
    for (auto index : layout.key_fields)
        columns.push_back(key_column_batches(source, index));

    constexpr auto no_position = std::numeric_limits<std::size_t>::max();
    auto next_rows = std::vector<std::size_t>(source.size, no_position);
    auto rows_by_hash = std::unordered_map<std::size_t, std::size_t>{};
    rows_by_hash.reserve(source.size);
    auto key = std::vector<std::optional<i64>>(columns.size());

    for (auto position = std::size_t{}; position != source.size; ++position) {
        auto hash = std::size_t{};
        for (auto index = std::size_t{}; index != columns.size(); ++index) {
            key[index] = columns[index].next_cell(position);
            const auto cell_hash = key[index] ? std::hash<i64>{}(*key[index]) : null_key_hash;
            hash = combine_frame_key_hash(hash, cell_hash);
        }

        const auto [entry, inserted] = rows_by_hash.try_emplace(hash, position);
        if (inserted)
            continue;

        for (auto previous = entry->second; previous != no_position; previous = next_rows[previous]) {
            auto equal = true;
            for (auto index = std::size_t{}; index != columns.size(); ++index) {
                if (columns[index].cell_at(previous) != key[index]) {
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
    details::register_frame_literal_type(type);
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
        const auto &runtime = details::canonical_frame_series_runtime_type(
            type, [](const TimeSeriesType &series_type) -> std::unique_ptr<TimeSeriesRuntimeType> {
                return std::make_unique<details::NativeFrameSeriesRuntimeType>(series_type);
            });
        register_time_series_runtime_type(runtime);
        frame_series_scalar_runtime_type(static_cast<const FrameSeriesRuntimeType &>(runtime));
    }
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
