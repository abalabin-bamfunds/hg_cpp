#include <bg/type/time_series_bundle_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/variable.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg {
namespace {

class ResolvedTimeSeriesBundleType final : public TimeSeriesBundleType {
  public:
    ResolvedTimeSeriesBundleType(const Schema &schema, TimeSeriesGuarantee guarantee)
        : TimeSeriesBundleType(schema, guarantee) {}

    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

bool is_time_series_schema(const Schema &schema) {
    return std::ranges::all_of(schema.fields(), [](const SchemaField &field) {
        return !field.optional && type_cast<TimeSeriesType>(field.type) != nullptr;
    });
}

void validate_time_series_schema_type(const SchemaType &schema) {
    if (const auto *descriptor = dynamic_cast<const Schema *>(&schema)) {
        if (!is_time_series_schema(*descriptor)) {
            throw std::invalid_argument{"a time series bundle schema requires present time-series fields"};
        }
        return;
    }
    if (dynamic_cast<const Variable *>(&schema) == nullptr) {
        throw std::invalid_argument{"a time series bundle requires a schema descriptor or schema variable"};
    }
}

const Schema &schema_with_minimum_guarantee(const Schema &schema, TimeSeriesGuarantee guarantee) {
    auto fields = std::vector<SchemaField>{};
    fields.reserve(schema.direct_fields().size());
    for (const auto &field : schema.direct_fields()) {
        const auto &time_series = static_cast<const TimeSeriesType &>(*field.type);
        fields.push_back(
            {field.name, &time_series.with_guarantee(strongest(*time_series.guarantee().value(), guarantee)), false});
    }

    const auto *base = schema.base() == nullptr ? nullptr : &schema_with_minimum_guarantee(*schema.base(), guarantee);
    return bg::schema(schema.name(), schema.identity(), base, fields);
}

} // namespace

TimeSeriesBundleType::TimeSeriesBundleType(const SchemaType &schema, GuaranteeParameter guarantee)
    : FixedItemTimeSeriesType(guarantee), schema_{&schema} {
    validate_time_series_schema_type(schema);
}

const TimeSeriesBundleType &TimeSeriesBundleType::from_fields(std::span<const SchemaField> fields,
                                                              TimeSeriesGuarantee guarantee) {
    if (std::ranges::any_of(fields, [](const auto &field) {
            return field.type == nullptr || !field.type->is_resolved() || field.optional ||
                   type_cast<TimeSeriesType>(field.type) == nullptr;
        })) {
        throw std::invalid_argument{"a time series bundle type requires resolved time-series fields"};
    }

    return time_series_bundle_type(anonymous_schema(fields), guarantee);
}

const Schema &TimeSeriesBundleType::schema() const {
    const auto *schema = dynamic_cast<const Schema *>(schema_);
    if (schema == nullptr) {
        const auto &variable = dynamic_cast<const Variable &>(*schema_);
        throw TypeResolutionError{TypeResolutionErrorCode::unmapped_variable, variable.name(),
                                  "schema variable has no type map entry"};
    }
    return *schema;
}

bool TimeSeriesBundleType::contains_reference() const {
    if (!is_resolved()) {
        return false;
    }
    for (auto index = std::size_t{}; index < item_count(); ++index) {
        if (item_type(index).contains_reference()) {
            return true;
        }
    }
    return false;
}

std::size_t TimeSeriesBundleType::item_count() const noexcept {
    return schema_->field_count();
}

const TimeSeriesType &TimeSeriesBundleType::item_type(std::size_t index) const {
    return static_cast<const TimeSeriesType &>(*schema().field(index).type);
}

util::symbol TimeSeriesBundleType::item_name(std::size_t index) const {
    return schema().field(index).name;
}

std::optional<std::size_t> TimeSeriesBundleType::find_item_index(util::symbol name) const {
    for (auto index = std::size_t{}; index < item_count(); ++index) {
        if (item_name(index) == name) {
            return index;
        }
    }
    return std::nullopt;
}

const TimeSeriesType &TimeSeriesBundleType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_bundle_type(schema(), guarantee);
}

bool TimeSeriesBundleType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *bundle = type_cast<TimeSeriesBundleType>(&supplied);
    if (bundle == nullptr || !bundle->is_resolved()) {
        return false;
    }

    auto trial = type_map.overlay();
    if (const auto *parameter_schema = dynamic_cast<const Schema *>(schema_)) {
        if (parameter_schema->named() && bundle->schema().named() &&
            !bundle->schema().derives_from(*parameter_schema)) {
            return false;
        }

        for (const auto &parameter : parameter_schema->fields()) {
            const auto *argument = bundle->schema().find(parameter.name);
            if (argument == nullptr) {
                return false;
            }

            const auto &parameter_type = static_cast<const TimeSeriesType &>(*parameter.type);
            const auto &argument_type = static_cast<const TimeSeriesType &>(*argument->type);
            if (!parameter_type.match(argument_type, trial)) {
                return false;
            }
        }
    } else if (!schema_->match(bundle->schema(), trial)) {
        return false;
    }
    if (!guarantee().match(*bundle->guarantee().value(), trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesBundleType::resolve(const TypeMap &type_map) const {
    const auto &resolved = schema_type().resolve(type_map);
    const auto *schema = dynamic_cast<const Schema *>(&resolved);
    if (schema == nullptr || !schema->is_resolved() || !is_time_series_schema(*schema)) {
        auto subject = util::symbol{};
        if (const auto *variable = dynamic_cast<const Variable *>(&schema_type())) {
            subject = variable->name();
        } else if (const auto *owner = dynamic_cast<const Schema *>(&schema_type())) {
            subject = owner->name();
        } else if (schema != nullptr) {
            subject = schema->name();
        }
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, subject,
                                  "schema did not resolve to present time-series fields"};
    }
    return time_series_bundle_type(*schema, guarantee().resolve(type_map));
}

const TimeSeriesBundleType &time_series_bundle_type(const Schema &schema, TimeSeriesGuarantee guarantee) {
    if (!schema.is_resolved() || !is_time_series_schema(schema)) {
        throw std::invalid_argument{"a time series bundle requires resolved, present time-series fields"};
    }

    struct Key {
        const Schema *schema;
        TimeSeriesGuarantee guarantee;

        bool operator==(const Key &) const = default;
    };

    static auto mutex = std::mutex{};
    static auto types = std::vector<std::pair<Key, const ResolvedTimeSeriesBundleType *>>{};

    const auto &effective_schema = schema_with_minimum_guarantee(schema, guarantee);
    const auto lock = std::lock_guard{mutex};
    const auto key = Key{&effective_schema, guarantee};
    const auto existing = std::ranges::find(types, key, &decltype(types)::value_type::first);
    if (existing != types.end()) {
        return *existing->second;
    }

    types.emplace_back(key, nullptr);
    try {
        const auto &type = TypeRegistry::instance().register_owned(
            std::make_unique<ResolvedTimeSeriesBundleType>(effective_schema, guarantee));
        types.back().second = &type;
        return type;
    } catch (...) {
        types.pop_back();
        throw;
    }
}

} // namespace bg
