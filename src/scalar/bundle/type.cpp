#include <bg/scalar/bundle/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
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

HierarchicalScalarType::HierarchicalScalarType(util::symbol name) noexcept : name_{name}, base_{nullptr} {}

void HierarchicalScalarType::initialize(const Schema &schema) {
    if (!schema.is_resolved() || std::ranges::any_of(schema.fields(), [](const auto &field) {
            return type_cast<ScalarType>(field.type) == nullptr;
        })) {
        throw std::invalid_argument{"a hierarchical scalar schema requires resolved scalar fields"};
    }

    schema_ = &schema;
}

bool HierarchicalScalarType::finalize_capabilities() noexcept {
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

bool HierarchicalScalarType::derives_from(const HierarchicalScalarType &type) const noexcept {
    for (auto current = this; current != nullptr; current = current->base()) {
        if (current == &type) {
            return true;
        }
    }
    return false;
}

bool HierarchicalScalarType::supports_equality() const noexcept {
    return supports_equality_;
}

bool HierarchicalScalarType::supports_hash() const noexcept {
    return supports_hash_;
}

bool HierarchicalScalarType::do_match(const Type &supplied, TypeMap &) const {
    const auto *hierarchical = type_cast<HierarchicalScalarType>(&supplied);
    return hierarchical != nullptr && hierarchical->derives_from(*this);
}

namespace details {

namespace {

enum class HierarchicalTypeStatus : std::uint8_t {
    reserved,
    finalized,
};

struct HierarchicalTypeEntry {
    const HierarchicalScalarType *type;
    HierarchicalTypeStatus status;
};

enum class RequiredFieldVisit : std::uint8_t {
    active,
    complete,
};

bool has_required_field_cycle(
    const HierarchicalScalarType &type,
    std::unordered_map<const HierarchicalScalarType *, RequiredFieldVisit> &visits) {
    const auto [entry, inserted] = visits.try_emplace(&type, RequiredFieldVisit::active);
    if (!inserted) {
        return entry->second == RequiredFieldVisit::active;
    }

    for (const auto &field : type.fields()) {
        const auto *nested = field.optional ? nullptr : type_cast<HierarchicalScalarType>(field.type);
        if (nested != nullptr && has_required_field_cycle(*nested, visits)) {
            return true;
        }
    }

    visits.at(&type) = RequiredFieldVisit::complete;
    return false;
}

void validate_required_field_cycles(const HierarchicalScalarType &type) {
    auto visits = std::unordered_map<const HierarchicalScalarType *, RequiredFieldVisit>{};
    if (has_required_field_cycle(type, visits)) {
        throw std::invalid_argument{"required hierarchical scalar fields cannot form a recursive cycle"};
    }
}

} // namespace

const HierarchicalScalarType &canonical_hierarchical_scalar_type(
    const std::type_info &identity, std::string_view name, void (*initialize)(HierarchicalScalarType &)) {
    static auto types = std::unordered_map<std::type_index, HierarchicalTypeEntry>{};

    const auto lock = std::lock_guard{semantic_type_registration_mutex()};
    const auto find_or_construct = [&]() -> const HierarchicalScalarType & {
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
                std::unique_ptr<HierarchicalScalarType>{new HierarchicalScalarType{util::symbol{name}}};
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
        HierarchicalScalarType *result = nullptr;
        try {
            auto type = std::unique_ptr<HierarchicalScalarType>{new HierarchicalScalarType{util::symbol{name}}};
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
        return *result;
    };

    if (SemanticTypeRegistration::current() != nullptr) {
        return find_or_construct();
    }

    const HierarchicalScalarType *result = nullptr;
    auto registration = SemanticTypeRegistration{};
    result = &find_or_construct();
    registration.commit();
    return *result;
}

} // namespace details
} // namespace bg
