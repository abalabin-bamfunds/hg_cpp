#include <bg/scalar/bundle/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/scope_exit.hpp>

#include "../../type/type_registration.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace bg {

void HierarchicalScalarType::format_name(std::string &result) const {
    if (is_resolved()) {
        schema_type().format_name(result);
        return;
    }

    result += "Bundle<";
    schema_type().format_name(result);
    result += '>';
}

GenericHierarchicalScalarType::GenericHierarchicalScalarType(const SchemaType &schema)
    : schema_{&schema} {}

bool ResolvedHierarchicalScalarType::derives_from(const ResolvedHierarchicalScalarType &type) const noexcept {
    for (auto current = this; current != nullptr; current = current->base()) {
        if (current == &type) {
            return true;
        }
    }
    return false;
}

bool ResolvedHierarchicalScalarType::do_match(const Type &supplied, TypeMap &) const {
    const auto *hierarchical = type_cast<HierarchicalScalarType>(&supplied);
    if (hierarchical == nullptr || !hierarchical->is_resolved()) {
        return false;
    }
    return static_cast<const ResolvedHierarchicalScalarType &>(*hierarchical).derives_from(*this);
}

bool GenericHierarchicalScalarType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *hierarchical = type_cast<HierarchicalScalarType>(&supplied);
    if (hierarchical == nullptr || !hierarchical->is_resolved()) {
        return false;
    }

    auto trial = type_map.overlay();
    if (!schema_->match(hierarchical->schema_type(), trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericHierarchicalScalarType::resolve(const TypeMap &type_map) const {
    const auto &resolved = schema_->resolve(type_map);
    const auto *schema = dynamic_cast<const Schema *>(&resolved);
    if (schema == nullptr || !schema->is_resolved() || std::ranges::any_of(schema->fields(), [](const auto &field) {
            return type_cast<ScalarType>(field.type) == nullptr;
        })) {
        auto subject = util::symbol{"HIERARCHICAL"};
        if (const auto *variable = dynamic_cast<const Variable *>(schema_)) {
            subject = variable->name();
        }
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, subject,
                                  "schema did not resolve to resolved scalar fields"};
    }

    return hierarchical_scalar_type(*schema);
}

void ResolvedHierarchicalScalarType::initialize(const Schema &schema) {
    schema_ = &schema;
}

bool ResolvedHierarchicalScalarType::finalize_capabilities() noexcept {
    const auto equality = std::ranges::all_of(fields(), [](const auto &field) {
        return static_cast<const ScalarType &>(*field.type).supports_equality();
    });
    const auto hash = std::ranges::all_of(fields(), [](const auto &field) {
        return static_cast<const ScalarType &>(*field.type).supports_hash();
    });
    const auto changed = equality != supports_equality_ || hash != supports_hash_;
    supports_equality_ = equality;
    supports_hash_ = hash;
    return changed;
}

namespace details {

namespace {

enum class HierarchicalTypeStatus : std::uint8_t {
    reserved,
    finalized,
};

struct HierarchicalTypeEntry {
    const ResolvedHierarchicalScalarType *type;
    HierarchicalTypeStatus status;
};

using HierarchicalSchemaAssociations =
    std::unordered_map<const Schema *, const ResolvedHierarchicalScalarType *>;

HierarchicalSchemaAssociations &hierarchical_schema_associations() {
    static auto associations = HierarchicalSchemaAssociations{};
    return associations;
}

enum class RequiredFieldVisit : std::uint8_t {
    active,
    complete,
};

bool has_required_field_cycle(
    const ResolvedHierarchicalScalarType &type,
    std::unordered_map<const ResolvedHierarchicalScalarType *, RequiredFieldVisit> &visits) {
    const auto [entry, inserted] = visits.try_emplace(&type, RequiredFieldVisit::active);
    if (!inserted) {
        return entry->second == RequiredFieldVisit::active;
    }

    for (const auto &field : type.fields()) {
        const auto *nested = field.optional ? nullptr : type_cast<HierarchicalScalarType>(field.type);
        if (nested != nullptr && nested->is_resolved() &&
            has_required_field_cycle(static_cast<const ResolvedHierarchicalScalarType &>(*nested), visits)) {
            return true;
        }
    }

    visits.at(&type) = RequiredFieldVisit::complete;
    return false;
}

void validate_required_field_cycles(const ResolvedHierarchicalScalarType &type) {
    auto visits = std::unordered_map<const ResolvedHierarchicalScalarType *, RequiredFieldVisit>{};
    if (has_required_field_cycle(type, visits)) {
        throw std::invalid_argument{"required hierarchical scalar fields cannot form a recursive cycle"};
    }
}

} // namespace

const ResolvedHierarchicalScalarType &canonical_hierarchical_scalar_type(
    const std::type_info &identity, std::string_view name, void (*initialize)(ResolvedHierarchicalScalarType &)) {
    static auto types = std::unordered_map<std::type_index, HierarchicalTypeEntry>{};

    const auto lock = std::lock_guard{semantic_type_registration_mutex()};
    const auto find_or_construct = [&]() -> const ResolvedHierarchicalScalarType & {
        const auto key = std::type_index{identity};
        if (const auto entry = types.find(key); entry != types.end()) {
            if (entry->second.status == HierarchicalTypeStatus::reserved) {
                return *entry->second.type;
            }

            const auto &existing = *entry->second.type;
            if (existing.name().text() != name) {
                throw std::logic_error{"conflicting hierarchical scalar declaration: " + std::string{name}};
            }

            entry->second.status = HierarchicalTypeStatus::reserved;
            const auto restore_status = util::scope_exit{[key]() noexcept {
                types.find(key)->second.status = HierarchicalTypeStatus::finalized;
            }};
            auto candidate_owner =
                std::unique_ptr<ResolvedHierarchicalScalarType>{new ResolvedHierarchicalScalarType{}};
            auto *candidate_type = candidate_owner.get();
            TypeRegistry::instance().register_owned(std::move(candidate_owner));
            auto &candidate = *candidate_type;
            const auto remove_candidate = util::scope_exit{[&candidate]() noexcept {
                TypeRegistry::instance().rollback(candidate);
            }};
            initialize(candidate);
            if (existing.base() != candidate.base() || &existing.schema() != &candidate.schema()) {
                throw std::logic_error{"conflicting hierarchical scalar declaration: " + std::string{name}};
            }
            return existing;
        }

        auto [entry, inserted] = types.try_emplace(key, HierarchicalTypeEntry{nullptr,
                                                                             HierarchicalTypeStatus::reserved});
        ResolvedHierarchicalScalarType *result = nullptr;
        try {
            auto type = std::unique_ptr<ResolvedHierarchicalScalarType>{new ResolvedHierarchicalScalarType{}};
            auto *pending_type = type.get();
            TypeRegistry::instance().register_owned(std::move(type));
            result = pending_type;
            entry->second.type = result;
            auto &registration = *SemanticTypeRegistration::current();
            registration.on_finalize([result] { return result->finalize_capabilities(); });
            registration.on_validate([result] { validate_required_field_cycles(*result); });
            registration.on_commit([key] { types.at(key).status = HierarchicalTypeStatus::finalized; });
            registration.on_rollback([key, result] {
                types.erase(key);
                TypeRegistry::instance().rollback(*result);
            });
        } catch (...) {
            types.erase(key);
            if (result != nullptr) {
                TypeRegistry::instance().rollback(*result);
            }
            throw;
        }

        initialize(*result);

        auto &associations = hierarchical_schema_associations();
        const auto [association, association_inserted] = associations.try_emplace(&result->schema(), result);
        if (!association_inserted && association->second != result) {
            throw std::logic_error{"a schema already has a different hierarchical scalar type"};
        }
        if (association_inserted) {
            auto remove_association = util::scope_exit{[&associations, schema = &result->schema()]() noexcept {
                associations.erase(schema);
            }};
            SemanticTypeRegistration::current()->on_rollback([&associations, schema = &result->schema()] {
                associations.erase(schema);
            });
            remove_association.release();
        }
        return *result;
    };

    if (SemanticTypeRegistration::current() != nullptr) {
        return find_or_construct();
    }

    const ResolvedHierarchicalScalarType *result = nullptr;
    auto registration = SemanticTypeRegistration{};
    result = &find_or_construct();
    registration.commit();
    return *result;
}

const ResolvedHierarchicalScalarType &canonical_hierarchical_scalar_type(const Schema &schema) {
    const auto lock = std::lock_guard{semantic_type_registration_mutex()};
    auto &associations = hierarchical_schema_associations();
    if (const auto found = associations.find(&schema); found != associations.end()) {
        return *found->second;
    }
    if (std::holds_alternative<std::type_index>(schema.identity())) {
        throw std::logic_error{"a C++-identified schema requires prior hierarchical scalar registration"};
    }
    if ((!schema.is_finalized() && SemanticTypeRegistration::current() == nullptr) ||
        (schema.is_finalized() && (!schema.is_resolved() || std::ranges::any_of(schema.fields(), [](const auto &field) {
             return type_cast<ScalarType>(field.type) == nullptr;
         })))) {
        throw std::invalid_argument{"a hierarchical scalar schema requires resolved scalar fields"};
    }

    const auto find_or_construct = [&]() -> const ResolvedHierarchicalScalarType & {
        if (const auto found = associations.find(&schema); found != associations.end()) {
            return *found->second;
        }

        const ResolvedHierarchicalScalarType *base = nullptr;
        if (schema.base() != nullptr) {
            base = &canonical_hierarchical_scalar_type(*schema.base());
        }

        auto type_owner = std::unique_ptr<ResolvedHierarchicalScalarType>{new ResolvedHierarchicalScalarType{}};
        auto *type = type_owner.get();
        TypeRegistry::instance().register_owned(std::move(type_owner));
        auto rollback_type = util::scope_exit{[type]() noexcept { TypeRegistry::instance().rollback(*type); }};

        type->set_base(base);
        type->initialize(schema);
        associations.emplace(&schema, type);
        auto remove_association = util::scope_exit{[&associations, schema = &schema, type]() noexcept {
            const auto found = associations.find(schema);
            if (found != associations.end() && found->second == type) {
                associations.erase(found);
            }
        }};

        auto &registration = *SemanticTypeRegistration::current();
        registration.on_finalize([type] { return type->finalize_capabilities(); });
        registration.on_validate([type] {
            if (!type->schema().is_resolved() || std::ranges::any_of(type->fields(), [](const auto &field) {
                    return type_cast<ScalarType>(field.type) == nullptr;
                })) {
                throw std::invalid_argument{"a hierarchical scalar schema requires resolved scalar fields"};
            }
        });
        registration.on_validate([type] { validate_required_field_cycles(*type); });
        registration.on_rollback([&associations, schema = &schema, type] {
            const auto found = associations.find(schema);
            if (found != associations.end() && found->second == type) {
                associations.erase(found);
            }
            TypeRegistry::instance().rollback(*type);
        });
        rollback_type.release();
        remove_association.release();
        return *type;
    };

    if (SemanticTypeRegistration::current() != nullptr) {
        return find_or_construct();
    }

    const ResolvedHierarchicalScalarType *result = nullptr;
    auto registration = SemanticTypeRegistration{};
    result = &find_or_construct();
    registration.commit();
    return *result;
}

} // namespace details

const ResolvedHierarchicalScalarType &hierarchical_scalar_type(const Schema &schema) {
    return details::canonical_hierarchical_scalar_type(schema);
}

} // namespace bg
