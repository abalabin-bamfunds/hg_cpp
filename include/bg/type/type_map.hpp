#pragma once

#include <bg/type/size.hpp>
#include <bg/type/time_series_guarantee.hpp>
#include <bg/type/type.hpp>
#include <bg/type/variable.hpp>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <variant>

namespace bg {

using TypeMapValue = std::variant<const Type *, TimeSeriesGuarantee, Size>;

template <typename Category> class TypeVariableType;

class TypeMap {
  public:
    TypeMap() = default;
    TypeMap(const TypeMap &) = delete;
    TypeMap &operator=(const TypeMap &) = delete;
    TypeMap(TypeMap &&) = default;
    TypeMap &operator=(TypeMap &&) = default;

    const TypeMapValue *find(const Variable &variable) const {
        const auto found = values_.find(&variable);

        if (found != values_.end()) {
            return &found->second;
        }

        if (parent_ != nullptr) {
            return parent_->find(variable);
        }

        return missing_mapping_ == nullptr
                   ? nullptr
                   : missing_mapping_(missing_mapping_context_, const_cast<TypeMap &>(*this), variable);
    }

    void set(const Variable &variable, TypeMapValue value) { values_.insert_or_assign(&variable, std::move(value)); }

    [[nodiscard]] TypeMap overlay() const { return TypeMap{this}; }

    [[nodiscard]] TypeMap copy() const {
        auto result = TypeMap{};
        copy_into(result);
        result.matching_structure_only_ = matching_structure_only();
        result.matching_reference_target_ = matching_reference_target_;
        return result;
    }

    void apply(TypeMap &&overlay) {
        for (auto &[variable, value] : overlay.values_) {
            set(*variable, std::move(value));
        }
    }

    bool has_same_mappings(const TypeMap &other) const {
        if (parent_ == nullptr && other.parent_ == nullptr) {
            return values_ == other.values_;
        }

        const auto left = copy();
        const auto right = other.copy();
        return left.values_ == right.values_;
    }

    bool includes(const TypeMap &required) const {
        const auto mappings = required.copy();
        return std::ranges::all_of(mappings.values_, [&](const auto &entry) {
            const auto *value = find(*entry.first);
            return value != nullptr && *value == entry.second;
        });
    }

    bool matching_structure_only() const noexcept {
        return matching_structure_only_ || (parent_ != nullptr && parent_->matching_structure_only());
    }

    bool matching_reference_target() const noexcept { return matching_reference_target_; }

    [[nodiscard]] TypeMap reference_target_overlay() const {
        auto result = overlay();
        result.matching_reference_target_ = true;
        return result;
    }

  private:
    friend class Signature;
    template <typename Category> friend class TypeVariableType;

    using MissingMappingFunction = const TypeMapValue *(*)(void *, TypeMap &, const Variable &);

    explicit TypeMap(const TypeMap *parent) : parent_(parent) {}

    [[nodiscard]] bool apply_compatible(TypeMap &&overlay) {
        const auto compatible = std::ranges::all_of(overlay.values_, [&](const auto &entry) {
            const auto *current = find(*entry.first);
            return current == nullptr || *current == entry.second;
        });
        if (!compatible) {
            return false;
        }

        apply(std::move(overlay));
        return true;
    }

    void missing_mapping_handler(void *context, MissingMappingFunction function) noexcept {
        missing_mapping_context_ = context;
        missing_mapping_ = function;
    }

    void copy_into(TypeMap &destination) const {
        if (parent_ != nullptr) {
            parent_->copy_into(destination);
        }
        for (const auto &[variable, value] : values_) {
            destination.set(*variable, value);
        }
    }

    const TypeMap *parent_{};
    std::unordered_map<const Variable *, TypeMapValue> values_;
    bool matching_structure_only_{};
    bool matching_reference_target_{};
    void *missing_mapping_context_{};
    MissingMappingFunction missing_mapping_{};
};

} // namespace bg
