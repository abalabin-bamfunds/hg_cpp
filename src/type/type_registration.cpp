#include "type_registration.hpp"

#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace bg::details {
namespace {

thread_local SemanticTypeRegistration *active_registration = nullptr;

enum class VisitState : unsigned char {
    active,
    complete,
};

bool has_required_field_cycle(
    const ResolvedHierarchicalScalarType &type,
    std::unordered_map<const ResolvedHierarchicalScalarType *, VisitState> &visited);

bool has_required_field_cycle(
    const ScalarType &type,
    std::unordered_map<const ResolvedHierarchicalScalarType *, VisitState> &visited) {
    if (const auto *hierarchical = type_cast<HierarchicalScalarType>(&type)) {
        return hierarchical->is_resolved() &&
               has_required_field_cycle(static_cast<const ResolvedHierarchicalScalarType &>(*hierarchical), visited);
    }

    const auto *tuple = type_cast<TupleScalarType>(&type);
    if (tuple == nullptr || !tuple->is_resolved()) {
        return false;
    }
    return std::ranges::any_of(tuple->item_types(), [&](const auto *item) {
        return has_required_field_cycle(*item, visited);
    });
}

bool has_required_field_cycle(
    const ResolvedHierarchicalScalarType &type,
    std::unordered_map<const ResolvedHierarchicalScalarType *, VisitState> &visited) {
    const auto [entry, inserted] = visited.try_emplace(&type, VisitState::active);
    if (!inserted) {
        return entry->second == VisitState::active;
    }

    for (const auto &field : type.fields()) {
        if (!field.optional && has_required_field_cycle(static_cast<const ScalarType &>(*field.type), visited)) {
            return true;
        }
    }

    visited.at(&type) = VisitState::complete;
    return false;
}

bool has_required_field_cycle(const ResolvedHierarchicalScalarType &type) {
    auto visited = std::unordered_map<const ResolvedHierarchicalScalarType *, VisitState>{};
    return has_required_field_cycle(type, visited);
}

} // namespace

SemanticTypeRegistration::SemanticTypeRegistration() {
    if (active_registration != nullptr) {
        throw std::logic_error{"semantic type registrations cannot be nested"};
    }
    active_registration = this;
}

SemanticTypeRegistration::~SemanticTypeRegistration() {
    active_registration = nullptr;
}

SemanticTypeRegistration *SemanticTypeRegistration::current() noexcept {
    return active_registration;
}

void SemanticTypeRegistration::add_hierarchical_type(ResolvedHierarchicalScalarType &type) {
    hierarchical_types_.push_back(&type);
}

void SemanticTypeRegistration::require_hash(const ScalarType &type, HashRequirement requirement) {
    hash_requirements_.push_back({&type, requirement});
}

void SemanticTypeRegistration::complete() {
    for (auto *type : hierarchical_types_) {
        if (!type->schema().is_resolved() || std::ranges::any_of(type->fields(), [](const auto &field) {
                return type_cast<ScalarType>(field.type) == nullptr;
            })) {
            throw std::invalid_argument{"a hierarchical scalar schema requires resolved scalar fields"};
        }
    }

    auto changed = true;
    while (changed) {
        changed = false;
        for (auto *type : hierarchical_types_) {
            changed = type->finalize_capabilities() || changed;
        }
    }

    for (auto *type : hierarchical_types_) {
        if (has_required_field_cycle(*type)) {
            throw std::invalid_argument{"required hierarchical scalar fields cannot form a recursive cycle"};
        }
    }

    for (const auto &[type, requirement] : hash_requirements_) {
        if (type->supports_hash()) {
            continue;
        }
        if (requirement == HashRequirement::set_item) {
            throw std::invalid_argument{"set item type does not support hashing"};
        }
        if (requirement == HashRequirement::dictionary_key) {
            throw std::invalid_argument{"dictionary key type does not support hashing"};
        }
        if (requirement == HashRequirement::time_series_set_item) {
            throw std::invalid_argument{"time series set item type does not support hashing"};
        }
        throw std::invalid_argument{"time series dictionary key type does not support hashing"};
    }
}

} // namespace bg::details
