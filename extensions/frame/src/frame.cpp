#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/frame/literal.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/scope_exit.hpp>

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

auto canonical_series_runtime_mutex = std::mutex{};
auto native_frame_layout_mutex = std::mutex{};
auto canonical_series_runtimes =
    std::unordered_map<const TimeSeriesType *, std::unique_ptr<TimeSeriesRuntimeType>>{};
auto canonical_series_scalar_runtimes =
    std::unordered_map<const FrameSeriesRuntimeType *, std::unique_ptr<ScalarRuntimeType>>{};
auto canonical_snapshot_runtimes =
    std::unordered_map<const ScalarType *, std::unique_ptr<ScalarRuntimeType>>{};
auto native_frame_layouts = std::unordered_map<const SchemaType *, std::unique_ptr<details::NativeFrameLayout>>{};

void validate_frame_schema(const SchemaType &schema) {
    const auto *concrete = dynamic_cast<const Schema *>(std::addressof(schema));
    if (concrete == nullptr)
        return;
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

} // namespace

const details::NativeFrameLayout &details::native_frame_layout(const SchemaType &schema) {
    const auto lock = std::lock_guard{native_frame_layout_mutex};
    if (const auto found = native_frame_layouts.find(std::addressof(schema)); found != native_frame_layouts.end())
        return *found->second;

    const auto *concrete = dynamic_cast<const bg::Schema *>(std::addressof(schema));
    if (concrete == nullptr || !concrete->is_resolved())
        throw std::invalid_argument{"TSF native storage requires a resolved BG schema"};

    auto layout = std::make_unique<NativeFrameLayout>();
    layout->schema = concrete;
    layout->alignment = alignof(NativeFrameStorage);
    auto offset = sizeof(NativeFrameStorage);
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
    layout->size = (offset + layout->alignment - 1) / layout->alignment * layout->alignment;
    const auto *result = layout.get();
    native_frame_layouts.emplace(std::addressof(schema), std::move(layout));
    return *result;
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
    const auto lock = std::lock_guard{canonical_series_runtime_mutex};
    const auto *key = std::addressof(type);
    if (const auto entry = canonical_series_runtimes.find(key); entry != canonical_series_runtimes.end()) {
        if (&entry->second->type() != &type)
            throw std::logic_error{"frame runtime requested for a non-canonical schema type"};
        return *entry->second;
    }

    auto runtime = factory(type);
    const auto *result = runtime.get();
    canonical_series_runtimes.emplace(key, std::move(runtime));
    return *result;
}

} // namespace details

const ScalarRuntimeType &frame_series_scalar_runtime_type(const FrameSeriesRuntimeType &runtime) {
    const auto &schema = static_cast<const FrameSeriesType &>(runtime.type()).schema_type();
    const auto &snapshot_type = frame_type(schema);
    frame_snapshot_runtime_type(snapshot_type);

    if (const auto *cached = runtime.cached_scalar_runtime_type(); cached != nullptr)
        return *cached;
    const auto lock = std::lock_guard{canonical_series_runtime_mutex};
    const auto *key = std::addressof(runtime);
    if (const auto found = canonical_series_scalar_runtimes.find(key);
        found != canonical_series_scalar_runtimes.end()) {
        runtime.cache_scalar_runtime_type(*found->second);
        return *found->second;
    }
    auto scalar_runtime = std::make_unique<details::FrameSeriesScalarRuntimeType>(runtime, snapshot_type);
    const auto *result = scalar_runtime.get();
    canonical_series_scalar_runtimes.emplace(key, std::move(scalar_runtime));
    runtime.cache_scalar_runtime_type(*result);
    return *result;
}

const ScalarRuntimeType &frame_snapshot_runtime_type(const FrameType &type) {
    if (!type.is_resolved())
        throw std::invalid_argument{"frame snapshot runtime requires a resolved frame scalar type"};
    const auto lock = std::lock_guard{canonical_series_runtime_mutex};
    const auto *key = std::addressof(type);
    if (const auto found = canonical_snapshot_runtimes.find(key); found != canonical_snapshot_runtimes.end())
        return *found->second;
    auto scalar_runtime = std::make_unique<details::FrameSnapshotRuntimeType>(type);
    const auto *result = scalar_runtime.get();
    const auto entry = canonical_snapshot_runtimes.emplace(key, std::move(scalar_runtime)).first;
    auto rollback = util::scope_exit{[&]() noexcept { canonical_snapshot_runtimes.erase(entry); }};
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
