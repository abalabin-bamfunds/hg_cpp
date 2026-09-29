#pragma once

#include <bg/util/symbol.hpp>

#include <cstddef>
#include <functional>
#include <string_view>
#include <variant>

namespace bg::details {

// Identifies an inherited requirement by lexical name or owning service object.
class InheritedTimeSeriesIdentity {
  public:
    static InheritedTimeSeriesIdentity named(util::symbol name) { return InheritedTimeSeriesIdentity{name}; }
    static InheritedTimeSeriesIdentity object(const void *value) { return InheritedTimeSeriesIdentity{value}; }

    bool is_object() const noexcept { return std::holds_alternative<const void *>(value_); }

    std::size_t hash() const noexcept {
        if (const auto *name = std::get_if<util::symbol>(&value_)) {
            return std::hash<std::string_view>{}(name->text());
        }
        return std::hash<const void *>{}(std::get<const void *>(value_));
    }

    bool operator==(const InheritedTimeSeriesIdentity &) const noexcept = default;

  private:
    explicit InheritedTimeSeriesIdentity(util::symbol name) : value_{name} {}
    explicit InheritedTimeSeriesIdentity(const void *value) : value_{value} {}

    std::variant<util::symbol, const void *> value_;
};

} // namespace bg::details
