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
        return result;
    }

    void apply(TypeMap &&overlay) {
        for (auto &[variable, value] : overlay.values_) {
            set(*variable, std::move(value));
        }
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
    void *missing_mapping_context_{};
    MissingMappingFunction missing_mapping_{};
};

} // namespace bg
