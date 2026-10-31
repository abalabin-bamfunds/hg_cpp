#include <bg/type/time_series_bundle_type.hpp>

#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/variable.hpp>
#include <bg/util/static_storage.hpp>

#include <algorithm>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg {
namespace {

class ResolvedTimeSeriesBundleType final : public TimeSeriesBundleType {
  public:
    ResolvedTimeSeriesBundleType(const Schema &schema, TimeSeriesGuarantee guarantee)
        : TimeSeriesBundleType(schema, guarantee) {
        set_default_runtime_type_factory([](const TimeSeriesType &type) -> const TimeSeriesRuntimeType & {
            const auto &bundle = static_cast<const TimeSeriesBundleType &>(type);
            auto items = std::vector<const TimeSeriesRuntimeType *>{};
            items.reserve(bundle.item_count());
            for (auto index = std::size_t{}; index < bundle.item_count(); ++index) {
                items.push_back(&resolve_default_time_series_runtime_type(bundle.item_type(index)));
            }
            return details::time_series_bundle_runtime_type(bundle, items);
        });
    }

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
    return bg::schema(schema.name(), schema.identity(), base, fields, schema.direct_metadata());
}

} // namespace

void TimeSeriesBundleType::format_name(std::string &result) const {
    result += "TSB<";
    schema_type().format_name(result);
    append_guarantee(result);
    result += '>';
}

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

const TimeSeriesType &TimeSeriesBundleType::reference_free_type() const {
    if (!is_resolved()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"TSB"},
                                  "an unresolved time series type cannot be made reference-free"};
    }

    const auto rewrite_schema = [](auto &&self, const Schema &source) -> const Schema & {
        auto fields = std::vector<SchemaField>{};
        fields.reserve(source.direct_fields().size());
        for (const auto &field : source.direct_fields()) {
            const auto &type = static_cast<const TimeSeriesType &>(*field.type).reference_free_type();
            fields.push_back({field.name, &type, field.optional});
        }

        const auto *base = source.base() == nullptr ? nullptr : &self(self, *source.base());
        if (base == source.base() && std::ranges::equal(fields, source.direct_fields())) {
            return source;
        }

        return bg::schema(source.name(), source.identity(), base, fields, source.direct_metadata());
    };

    const auto &schema = rewrite_schema(rewrite_schema, this->schema());
    return &schema == &this->schema() ? *this : time_series_bundle_type(schema, *guarantee().value());
}

const TimeSeriesType &TimeSeriesBundleType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_bundle_type(schema(), guarantee);
}

const TimeSeriesType &TimeSeriesBundleType::with_guarantee_limit(TimeSeriesGuarantee limit) const {
    if (!is_resolved()) {
        return TimeSeriesType::with_guarantee_limit(limit);
    }

    const auto current = *guarantee().value();
    const auto &limited_schema = limit_schema_guarantees(schema(), limit);
    const auto limited_guarantee = weakest(current, limit);
    if (&limited_schema == &schema() && limited_guarantee == current) {
        return *this;
    }

    return time_series_bundle_type(limited_schema, limited_guarantee);
}

const TimeSeriesType *TimeSeriesBundleType::common_type(const TimeSeriesType &other) const {
    const auto *bundle = type_cast<TimeSeriesBundleType>(&other);
    if (bundle == nullptr || !is_resolved() || !bundle->is_resolved()) {
        return nullptr;
    }

    const auto *common = common_schema(schema(), bundle->schema());
    if (common == nullptr) {
        return nullptr;
    }

    const auto common_guarantee = weakest(*guarantee().value(), *bundle->guarantee().value());
    return &time_series_bundle_type(*common, common_guarantee);
}

const Schema *TimeSeriesBundleType::common_schema(const Schema &left, const Schema &right) {
    if (left.named() != right.named()) {
        const auto &named = left.named() ? left : right;
        const auto &anonymous = left.named() ? right : left;
        if (!std::ranges::equal(named.fields(), anonymous.fields(), {}, &SchemaField::name, &SchemaField::name)) {
            return nullptr;
        }

        const auto common_named_schema = [&](auto &&self, const Schema &declaration) -> const Schema * {
            const auto *common_base = declaration.base() == nullptr ? nullptr : self(self, *declaration.base());
            if (declaration.base() != nullptr && common_base == nullptr) {
                return nullptr;
            }

            auto fields = std::vector<SchemaField>{};
            fields.reserve(declaration.direct_fields().size());
            for (const auto &field : declaration.direct_fields()) {
                const auto &free_field = *anonymous.find(field.name);
                const auto &declared_type = static_cast<const TimeSeriesType &>(*field.type);
                const auto &free_type = static_cast<const TimeSeriesType &>(*free_field.type);
                const auto *common_field_type = declared_type.common_type(free_type);
                if (common_field_type == nullptr) {
                    return nullptr;
                }
                fields.push_back({field.name, common_field_type, field.optional});
            }

            return &bg::schema(declaration.name(), declaration.identity(), common_base, fields,
                               declaration.direct_metadata());
        };

        return common_named_schema(common_named_schema, named);
    }

    if (left.name() != right.name() || left.identity() != right.identity() ||
        left.direct_metadata() != right.direct_metadata() ||
        left.direct_fields().size() != right.direct_fields().size() ||
        (left.base() == nullptr) != (right.base() == nullptr)) {
        return nullptr;
    }

    const auto *common_base = left.base() == nullptr ? nullptr : common_schema(*left.base(), *right.base());
    if (left.base() != nullptr && common_base == nullptr) {
        return nullptr;
    }

    auto fields = std::vector<SchemaField>{};
    fields.reserve(left.direct_fields().size());
    for (auto index = std::size_t{}; index < left.direct_fields().size(); ++index) {
        const auto &left_field = left.direct_fields()[index];
        const auto &right_field = right.direct_fields()[index];
        if (left_field.name != right_field.name) {
            return nullptr;
        }

        const auto &left_type = static_cast<const TimeSeriesType &>(*left_field.type);
        const auto &right_type = static_cast<const TimeSeriesType &>(*right_field.type);
        const auto *common_field_type = left_type.common_type(right_type);
        if (common_field_type == nullptr) {
            return nullptr;
        }
        fields.push_back({left_field.name, common_field_type, false});
    }

    return &bg::schema(left.name(), left.identity(), common_base, fields, left.direct_metadata());
}

const Schema &TimeSeriesBundleType::limit_schema_guarantees(const Schema &descriptor,
                                                            TimeSeriesGuarantee limit) {
    auto fields = std::vector<SchemaField>{};
    fields.reserve(descriptor.direct_fields().size());
    for (const auto &field : descriptor.direct_fields()) {
        const auto &time_series = static_cast<const TimeSeriesType &>(*field.type);
        fields.push_back({field.name, &time_series.with_guarantee_limit(limit), false});
    }

    const auto *base = descriptor.base() == nullptr ? nullptr : &limit_schema_guarantees(*descriptor.base(), limit);
    return bg::schema(descriptor.name(), descriptor.identity(), base, fields, descriptor.direct_metadata());
}

bool TimeSeriesBundleType::do_match(const Type &supplied, TypeMap &type_map) const {
    return do_match(supplied, type_map, std::nullopt);
}

bool TimeSeriesBundleType::do_match(const Type &supplied, TypeMap &type_map,
                                    std::optional<TimeSeriesGuarantee> reference_limit) const {
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
            if (!parameter_type.match(argument_type, trial, reference_limit)) {
                return false;
            }
        }
    } else {
        const auto &observed_bundle = !reference_limit
                                          ? *bundle
                                          : static_cast<const TimeSeriesBundleType &>(
                                                bundle->with_guarantee_limit(*reference_limit));
        if (!schema_->match(observed_bundle.schema(), trial)) {
            return false;
        }
    }

    const auto supplied_guarantee = *bundle->guarantee().value();
    const auto observed_guarantee = reference_limit ? weakest(supplied_guarantee, *reference_limit)
                                                    : supplied_guarantee;
    if (!guarantee().match(observed_guarantee, trial)) {
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

    static auto types = util::static_storage<std::vector<std::pair<Key, const ResolvedTimeSeriesBundleType *>>>{};

    const auto &effective_schema = schema_with_minimum_guarantee(schema, guarantee);
    auto &cache = types.get();
    const auto key = Key{&effective_schema, guarantee};
    const auto existing = std::ranges::find(
        cache, key, &std::remove_reference_t<decltype(cache)>::value_type::first);
    if (existing != cache.end()) {
        return *existing->second;
    }

    const auto &type = TypeRegistry::instance().register_owned(
        std::make_unique<ResolvedTimeSeriesBundleType>(effective_schema, guarantee));
    cache.emplace_back(key, &type);
    return type;
}

} // namespace bg
