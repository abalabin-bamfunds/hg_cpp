#pragma once

#include <bg/type/size.hpp>
#include <bg/type/time_series_guarantee.hpp>
#include <bg/type/type.hpp>
#include <bg/type/variable.hpp>

#include <unordered_map>
#include <utility>
#include <variant>

namespace bg {

using TypeBinding = std::variant<const Type *, TimeSeriesGuarantee, Size>;

class TypeMap {
  public:
    TypeMap() = default;
    TypeMap(const TypeMap &) = delete;
    TypeMap &operator=(const TypeMap &) = delete;
    TypeMap(TypeMap &&) = default;
    TypeMap &operator=(TypeMap &&) = default;

    [[nodiscard]] const TypeBinding *find(const Variable &variable) const noexcept {
        const auto found = values_.find(&variable);

        if (found != values_.end()) {
            return &found->second;
        }

        return parent_ == nullptr ? nullptr : parent_->find(variable);
    }

    void set(const Variable &variable, TypeBinding value) {
        values_.insert_or_assign(&variable, std::move(value));
    }

    [[nodiscard]] TypeMap overlay() const { return TypeMap{this}; }

    void apply(TypeMap &&overlay) {
        for (auto &[variable, value] : overlay.values_) {
            set(*variable, std::move(value));
        }
    }

  private:
    explicit TypeMap(const TypeMap *parent) : parent_(parent) {}

    const TypeMap *parent_{};
    std::unordered_map<const Variable *, TypeBinding> values_;
};

} // namespace bg
