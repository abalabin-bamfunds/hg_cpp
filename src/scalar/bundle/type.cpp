#include <bg/scalar/bundle/scalar_type.hpp>

#include <bg/type/type_map.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace bg {

HierarchicalScalarType::HierarchicalScalarType(util::symbol name, const HierarchicalScalarType *base,
                                               std::span<const HierarchicalScalarField> direct_fields)
    : name_{name}, base_{base}, direct_fields_{direct_fields.begin(), direct_fields.end()} {
    if (base_ != nullptr) {
        fields_.assign(base_->fields().begin(), base_->fields().end());
    }

    for (const auto &field : direct_fields_) {
        const auto inherited = std::ranges::find(fields_, field.name, &HierarchicalScalarField::name);
        if (inherited == fields_.end()) {
            fields_.push_back(field);
        } else {
            *inherited = field;
        }
    }
}

const HierarchicalScalarField *HierarchicalScalarType::find(util::symbol name) const noexcept {
    const auto field = std::ranges::find(fields_, name, &HierarchicalScalarField::name);
    return field == fields_.end() ? nullptr : std::addressof(*field);
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
    return std::ranges::all_of(fields_, [](const auto &field) { return field.type->supports_equality(); });
}

bool HierarchicalScalarType::supports_hash() const noexcept {
    return std::ranges::all_of(fields_, [](const auto &field) { return field.type->supports_hash(); });
}

bool HierarchicalScalarType::do_match(const Type &supplied, TypeMap &) const {
    const auto *hierarchical = type_cast<HierarchicalScalarType>(&supplied);
    return hierarchical != nullptr && hierarchical->derives_from(*this);
}

namespace details {

const HierarchicalScalarType &canonical_hierarchical_scalar_type(
    const std::type_info &identity, std::string_view name, const HierarchicalScalarType *base,
    std::span<const HierarchicalScalarField> fields) {
    static auto mutex = std::mutex{};
    static auto types = std::unordered_map<std::type_index, std::unique_ptr<HierarchicalScalarType>>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = std::type_index{identity};
    if (const auto entry = types.find(key); entry != types.end()) {
        const auto &existing = *entry->second;
        const auto same_fields = std::ranges::equal(
            existing.direct_fields(), fields, [](const auto &left, const auto &right) {
                return left.name == right.name && left.type == right.type && left.optional == right.optional;
            });
        if (existing.name().text() != name || existing.base() != base || !same_fields) {
            throw std::logic_error{"conflicting hierarchical scalar declaration: " + std::string{name}};
        }
        return existing;
    }

    for (auto index = std::size_t{}; index < fields.size(); ++index) {
        const auto &field = fields[index];
        if (field.type == nullptr || !field.type->is_resolved()) {
            throw std::invalid_argument{"a hierarchical scalar requires resolved field types"};
        }
        if (std::ranges::find(fields.subspan(index + 1), field.name,
                              &HierarchicalScalarField::name) != fields.end()) {
            throw std::invalid_argument{"a hierarchical scalar has duplicate field names"};
        }

        const auto *inherited = base == nullptr ? nullptr : base->find(field.name);
        if (inherited == nullptr) {
            continue;
        }
        if (!inherited->type->accepts(*field.type)) {
            throw std::invalid_argument{
                "a redefined hierarchical scalar field must have a compatible narrower type"};
        }
        if (!inherited->optional && field.optional) {
            throw std::invalid_argument{"a required hierarchical scalar field cannot become optional"};
        }
    }

    auto type = std::unique_ptr<HierarchicalScalarType>{
        new HierarchicalScalarType{util::symbol{name}, base, fields}};
    const auto *result = type.get();
    types.emplace(key, std::move(type));
    return *result;
}

} // namespace details
} // namespace bg
