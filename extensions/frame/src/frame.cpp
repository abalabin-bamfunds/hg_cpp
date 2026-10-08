#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/type/type_registry.hpp>

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

auto canonical_mutex = std::mutex{};
auto canonical_series_runtimes = std::unordered_map<std::type_index, std::unique_ptr<TimeSeriesRuntimeType>>{};

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

} // namespace

const FrameType &frame_type(const SchemaType &schema) {
    validate_frame_schema(schema);
    return TypeRegistry::instance().get_or_create<FrameType, std::hash<const SchemaType *>>(
        std::addressof(schema), [&] { return std::make_unique<FrameType>(schema); });
}

const FrameSeriesType &frame_series_type(const SchemaType &schema, GuaranteeParameter guarantee) {
    validate_frame_schema(schema);
    const auto key = frame_series_key(schema, guarantee);
    return TypeRegistry::instance().get_or_create<FrameSeriesType, FrameSeriesKeyHash>(
        key, [&] { return std::make_unique<FrameSeriesType>(schema, std::move(guarantee)); });
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

    auto trial = type_map.overlay();
    if (!schema_->match(frame->schema_type(), trial))
        return false;
    type_map.apply(std::move(trial));
    return true;
}

const TimeSeriesType &FrameSeriesType::item_type(std::size_t index) const {
    return *item_types_.at(index);
}

const TimeSeriesType &FrameSeriesType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return frame_series_type(*schema_, guarantee);
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
    if (!schema_->match(frame->schema_type(), trial) ||
        !guarantee().match(*frame->guarantee().value(), trial))
        return false;
    type_map.apply(std::move(trial));
    return true;
}

namespace details {

const TimeSeriesRuntimeType &canonical_frame_series_runtime_type(const std::type_info &identity,
                                                                 const TimeSeriesType &type,
                                                                 frame_series_runtime_factory factory) {
    const auto lock = std::lock_guard{canonical_mutex};
    const auto key = std::type_index{identity};
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
} // namespace bg::frame
