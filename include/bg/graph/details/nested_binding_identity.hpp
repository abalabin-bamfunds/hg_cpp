#pragma once

#include <bg/util/symbol.hpp>

#include <cstddef>
#include <functional>
#include <string_view>
#include <variant>

namespace bg::details {

// Identifies a binding inherited through nested graph boundaries by lexical name or owning object.
class NestedBindingIdentity {
  public:
    static NestedBindingIdentity named(util::symbol name) { return NestedBindingIdentity{name}; }
    static NestedBindingIdentity object(const void *value) { return NestedBindingIdentity{value}; }

    bool is_object() const noexcept { return std::holds_alternative<const void *>(value_); }

    std::size_t hash() const noexcept {
        if (const auto *name = std::get_if<util::symbol>(&value_)) {
            return std::hash<std::string_view>{}(name->text());
        }
        return std::hash<const void *>{}(std::get<const void *>(value_));
    }

    bool operator==(const NestedBindingIdentity &) const noexcept = default;

  private:
    explicit NestedBindingIdentity(util::symbol name) : value_{name} {}
    explicit NestedBindingIdentity(const void *value) : value_{value} {}

    std::variant<util::symbol, const void *> value_;
};

} // namespace bg::details
