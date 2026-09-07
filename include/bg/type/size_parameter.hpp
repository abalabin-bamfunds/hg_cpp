#pragma once

#include <bg/type/size_variable.hpp>

#include <optional>
#include <span>
#include <variant>

namespace bg {

class SizeParameter {
  public:
    SizeParameter() = default;
    SizeParameter(std::size_t value) : value_{value} {}
    SizeParameter(const SizeVariable &variable) : value_{&variable} {}

    bool is_resolved() const noexcept { return std::holds_alternative<std::size_t>(value_); }

    std::optional<std::size_t> value() const noexcept {
        if (const auto *value = std::get_if<std::size_t>(&value_)) {
            return *value;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool match(std::size_t supplied, TypeMap &bindings) const {
        if (const auto size = value()) {
            return *size == supplied;
        }
        return std::get<const SizeVariable *>(value_)->match(supplied, bindings);
    }

    [[nodiscard]] std::size_t resolve(const TypeMap &bindings) const {
        if (const auto size = value()) {
            return *size;
        }
        return std::get<const SizeVariable *>(value_)->resolve(bindings);
    }

  private:
    std::variant<std::size_t, const SizeVariable *> value_{std::dynamic_extent};
};

} // namespace bg
