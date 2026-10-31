#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>
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

struct HierarchicalTypeEntry {
    const ResolvedHierarchicalScalarType *type;
    std::type_index declaration_identity;
    std::string name;
};

using HierarchicalSchemaAssociations =
    std::unordered_map<const Schema *, const ResolvedHierarchicalScalarType *>;

HierarchicalSchemaAssociations &hierarchical_schema_associations() {
    static auto associations = util::static_storage<HierarchicalSchemaAssociations>{};
    return associations.get();
}

} // namespace

const ResolvedHierarchicalScalarType &canonical_hierarchical_scalar_type(
    const std::type_info &identity, const std::type_info &declaration_identity, std::string_view name,
    void (*initialize)(ResolvedHierarchicalScalarType &)) {
    static auto types = util::static_storage<std::unordered_map<std::type_index, HierarchicalTypeEntry>>{};
    auto &cache = types.get();

    const auto find_or_construct = [&]() -> const ResolvedHierarchicalScalarType & {
        const auto key = std::type_index{identity};
        if (const auto entry = cache.find(key); entry != cache.end()) {
            if (entry->second.declaration_identity != std::type_index{declaration_identity} ||
                entry->second.name != name) {
                throw std::logic_error{"conflicting hierarchical scalar declaration: " + std::string{name}};
            }
            return *entry->second.type;
        }

        auto type = std::unique_ptr<ResolvedHierarchicalScalarType>{new ResolvedHierarchicalScalarType{}};
        auto *result = type.get();
        TypeRegistry::instance().register_owned(std::move(type));
        cache.try_emplace(key, HierarchicalTypeEntry{result, std::type_index{declaration_identity}, std::string{name}});
        SemanticTypeRegistration::current()->add_hierarchical_type(*result);

        initialize(*result);

        auto &associations = hierarchical_schema_associations();
        const auto [association, association_inserted] = associations.try_emplace(&result->schema(), result);
        if (!association_inserted && association->second != result) {
            throw std::logic_error{"a schema already has a different hierarchical scalar type"};
        }
        return *result;
    };

    if (SemanticTypeRegistration::current() != nullptr) {
        return find_or_construct();
    }

    auto registration = SemanticTypeRegistration{};
    const auto &result = find_or_construct();
    registration.complete();
    return result;
}

const ResolvedHierarchicalScalarType &canonical_hierarchical_scalar_type(const Schema &schema) {
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

        auto type_owner = std::unique_ptr<ResolvedHierarchicalScalarType>{new ResolvedHierarchicalScalarType{}};
        auto *type = type_owner.get();
        TypeRegistry::instance().register_owned(std::move(type_owner));

        const ResolvedHierarchicalScalarType *base = nullptr;
        if (schema.base() != nullptr) {
            base = &canonical_hierarchical_scalar_type(*schema.base());
        }
        type->set_base(base);
        type->initialize(schema);
        associations.emplace(&schema, type);
        SemanticTypeRegistration::current()->add_hierarchical_type(*type);
        return *type;
    };

    if (SemanticTypeRegistration::current() != nullptr) {
        return find_or_construct();
    }

    auto registration = SemanticTypeRegistration{};
    const auto &result = find_or_construct();
    registration.complete();
    return result;
}

} // namespace details

const ResolvedHierarchicalScalarType &hierarchical_scalar_type(const Schema &schema) {
    return details::canonical_hierarchical_scalar_type(schema);
}

} // namespace bg
