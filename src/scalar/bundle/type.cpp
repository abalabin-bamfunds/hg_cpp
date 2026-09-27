#include <bg/scalar/bundle/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/util/scope_exit.hpp>

#include "../type_registration.hpp"

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

void HierarchicalScalarType::initialize(const HierarchicalScalarType *base,
                                        std::span<const HierarchicalScalarField> direct_fields) {
    for (auto index = std::size_t{}; index < direct_fields.size(); ++index) {
        const auto &field = direct_fields[index];
        if (field.type == nullptr || !field.type->is_resolved()) {
            throw std::invalid_argument{"a hierarchical scalar requires resolved field types"};
        }
        if (std::ranges::find(direct_fields.subspan(index + 1), field.name,
                              &HierarchicalScalarField::name) != direct_fields.end()) {
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

    base_ = base;
    direct_fields_.assign(direct_fields.begin(), direct_fields.end());
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

bool HierarchicalScalarType::finalize_capabilities() noexcept {
    const auto equality = std::ranges::all_of(fields_, [](const auto &field) {
        return field.type->supports_equality();
    });
    const auto hash = std::ranges::all_of(fields_, [](const auto &field) {
        return field.type->supports_hash();
    });
    const auto changed = equality != supports_equality_ || hash != supports_hash_;
    supports_equality_ = equality;
    supports_hash_ = hash;
    return changed;
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
    std::unique_ptr<HierarchicalScalarType> type;
    HierarchicalTypeStatus status;
};

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
            auto candidate = HierarchicalScalarType{util::symbol{name}};
            initialize(candidate);
            const auto same_fields = std::ranges::equal(
                existing.direct_fields(), candidate.direct_fields(), [](const auto &left, const auto &right) {
                    return left.name == right.name && left.type == right.type && left.optional == right.optional;
                });
            if (existing.base() != candidate.base() || !same_fields) {
                throw std::logic_error{"conflicting hierarchical scalar declaration: " + std::string{name}};
            }
            return existing;
        }

        auto type = std::unique_ptr<HierarchicalScalarType>{new HierarchicalScalarType{util::symbol{name}}};
        auto *result = type.get();
        types.emplace(key, HierarchicalTypeEntry{std::move(type), HierarchicalTypeStatus::reserved});
        try {
            auto &registration = *SemanticTypeRegistration::current();
            registration.on_rollback([key] { types.erase(key); });
            registration.on_finalize([result] { return result->finalize_capabilities(); });
            registration.on_commit([key] { types.at(key).status = HierarchicalTypeStatus::finalized; });
        } catch (...) {
            types.erase(key);
            throw;
        }

        initialize(*types.at(key).type);
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
