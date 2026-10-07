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
    ResolvedTimeSeriesBundleType(const BundleSchema &schema, TimeSeriesGuarantee guarantee)
        : TimeSeriesBundleType(schema, guarantee) {}

    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

bool same_fields(const BundleSchema &schema, const BundleSchema *base, std::span<const BundleField> fields) {
    return schema.base() == base &&
           std::ranges::equal(schema.direct_fields(), fields, {}, &BundleField::name, &BundleField::name) &&
           std::ranges::equal(schema.direct_fields(), fields, {}, &BundleField::type, &BundleField::type);
}

void validate_fields(const BundleSchema *base, std::span<const BundleField> fields) {
    for (auto index = std::size_t{}; index < fields.size(); ++index) {
        for (auto other = index + 1; other < fields.size(); ++other) {
            if (fields[index].name == fields[other].name) {
                throw std::invalid_argument{"a time series bundle schema has duplicate field names"};
            }
        }

        const auto *inherited = base == nullptr ? nullptr : base->find(fields[index].name);
        if (inherited == nullptr || !inherited->type->is_resolved() || !fields[index].type->is_resolved()) {
            continue;
        }

        if (!inherited->type->accepts(*fields[index].type)) {
            throw std::invalid_argument{"a redefined time series bundle field must have a compatible narrower type"};
        }
    }
}

[[nodiscard]] const BundleSchema &resolved_schema(const BundleSchema &schema, const TypeMap &type_map) {
    auto fields = std::vector<BundleField>{};
    fields.reserve(schema.direct_fields().size());
    for (const auto &[name, type] : schema.direct_fields()) {
        const auto &resolved = type->resolve(type_map);
        const auto *time_series = type_cast<TimeSeriesType>(&resolved);
        if (time_series == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name,
                                      "bundle field did not resolve to a time series type"};
        }
        fields.push_back({name, time_series});
    }

    const auto *base = schema.base() == nullptr ? nullptr : &resolved_schema(*schema.base(), type_map);
    return bundle_schema(schema.name(), schema.identity(), base, fields);
}

const BundleSchema &schema_with_minimum_guarantee(const BundleSchema &schema, TimeSeriesGuarantee guarantee) {
    auto fields = std::vector<BundleField>{};
    fields.reserve(schema.direct_fields().size());
    for (const auto &[name, type] : schema.direct_fields()) {
        fields.push_back({name, &type->with_guarantee(strongest(*type->guarantee().value(), guarantee))});
    }

    const auto *base = schema.base() == nullptr ? nullptr : &schema_with_minimum_guarantee(*schema.base(), guarantee);
    return bundle_schema(schema.name(), schema.identity(), base, fields);
}

} // namespace

const TimeSeriesBundleType &TimeSeriesBundleType::from_fields(std::span<const BundleField> fields,
                                                              TimeSeriesGuarantee guarantee) {
    if (std::ranges::any_of(fields,
                            [](const auto &field) { return field.type == nullptr || !field.type->is_resolved(); })) {
        throw std::invalid_argument{"a time series bundle type requires resolved field types"};
    }

    return time_series_bundle_type(unnamed_bundle_schema(fields), guarantee);
}

BundleSchema::BundleSchema(util::symbol name, const void *identity, const BundleSchema *base,
                           std::vector<BundleField> fields)
    : name_{name}, identity_{identity}, base_{base}, fields_{std::move(fields)} {
    if (base_ != nullptr) {
        all_fields_.reserve(base_->field_count() + fields_.size());
        for (auto index = std::size_t{}; index < base_->field_count(); ++index) {
            all_fields_.push_back(base_->field(index));
        }
    }

    for (const auto &field : fields_) {
        const auto inherited = std::ranges::find(all_fields_, field.name, &BundleField::name);
        if (inherited == all_fields_.end()) {
            all_fields_.push_back(field);
        } else {
            *inherited = field;
        }
    }
}

std::size_t BundleSchema::field_count() const noexcept {
    return all_fields_.size();
}

const BundleField &BundleSchema::field(std::size_t index) const {
    return all_fields_.at(index);
}

const BundleField *BundleSchema::find(util::symbol name) const noexcept {
    const auto field = std::ranges::find(all_fields_, name, &BundleField::name);
    return field == all_fields_.end() ? nullptr : std::addressof(*field);
}

bool BundleSchema::derives_from(const BundleSchema &schema) const noexcept {
    for (auto current = this; current != nullptr; current = current->base()) {
        if (current->identity() == schema.identity()) {
            return true;
        }
    }
    return false;
}

bool BundleSchema::is_resolved() const noexcept {
    return (base_ == nullptr || base_->is_resolved()) &&
           std::ranges::all_of(fields_, [](const auto &field) { return field.type->is_resolved(); });
}

const Type &BundleSchema::resolve(const TypeMap &type_map) const {
    return is_resolved() ? *this : resolved_schema(*this, type_map);
}

const BundleSchema &TimeSeriesBundleType::schema() const {
    const auto *schema = dynamic_cast<const BundleSchema *>(schema_);
    if (schema == nullptr) {
        const auto &variable = dynamic_cast<const Variable &>(*schema_);
        throw TypeResolutionError{TypeResolutionErrorCode::unmapped_variable, variable.name(),
                                  "bundle schema variable has no type map entry"};
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
    return *schema().field(index).type;
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
    if (const auto *parameter_schema = dynamic_cast<const BundleSchema *>(schema_)) {
        if (parameter_schema->named() && bundle->schema().named() &&
            !bundle->schema().derives_from(*parameter_schema)) {
            return false;
        }

        for (auto index = std::size_t{}; index < parameter_schema->field_count(); ++index) {
            const auto &parameter = parameter_schema->field(index);
            const auto *argument = bundle->schema().find(parameter.name);
            if (argument == nullptr || !parameter.type->match(*argument->type, trial)) {
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
    const auto *schema = dynamic_cast<const BundleSchema *>(&resolved);
    if (schema == nullptr || !schema->is_resolved()) {
        const auto &variable = dynamic_cast<const Variable &>(schema_type());
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, variable.name(),
                                  "bundle schema did not resolve to a concrete schema"};
    }
    return time_series_bundle_type(*schema, guarantee().resolve(type_map));
}

const BundleSchema &bundle_schema(util::symbol name, const void *identity, const BundleSchema *base,
                                  std::span<const BundleField> fields) {
    static auto mutex = std::mutex{};
    static auto schemas = std::vector<const BundleSchema *>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing = std::ranges::find_if(schemas, [&](const auto &candidate) {
        return candidate->name() == name && candidate->identity() == identity && same_fields(*candidate, base, fields);
    });
    if (existing != schemas.end()) {
        return **existing;
    }

    validate_fields(base, fields);
    const auto &schema = TypeRegistry::instance().register_owned(
        std::make_unique<BundleSchema>(name, identity, base, std::vector<BundleField>{fields.begin(), fields.end()}));
    try {
        schemas.push_back(&schema);
        return schema;
    } catch (...) {
        TypeRegistry::instance().rollback(schema);
        throw;
    }
}

const BundleSchema &unnamed_bundle_schema(std::span<const BundleField> fields) {
    return bundle_schema(util::symbol{""}, nullptr, nullptr, fields);
}

const TimeSeriesBundleType &time_series_bundle_type(const BundleSchema &schema, TimeSeriesGuarantee guarantee) {
    struct Key {
        const BundleSchema *schema;
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
