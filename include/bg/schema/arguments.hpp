#pragma once

#include <bg/util/named_argument.hpp>
#include <bg/util/symbol.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename Argument>
std::optional<util::symbol> schema_argument_name() {
    if constexpr (is_named_argument_v<Argument>) {
        return util::symbol{std::remove_cvref_t<Argument>::name.view()};
    } else {
        return std::nullopt;
    }
}

template <typename Argument>
decltype(auto) schema_argument_value(Argument &&argument) {
    if constexpr (is_named_argument_v<Argument>) {
        return std::forward<Argument>(argument).value;
    } else {
        return std::forward<Argument>(argument);
    }
}

template <typename Argument>
using schema_argument_value_t =
    std::remove_cvref_t<decltype(schema_argument_value(std::declval<Argument>()))>;

template <std::size_t FieldCount, typename Error = std::invalid_argument> class SchemaArgumentAssignment {
  public:
    template <typename FindField>
    std::size_t add(std::optional<util::symbol> name, FindField &&find_field) {
        if (!name) {
            if (named_) {
                throw Error{"a positional schema argument cannot follow a named argument"};
            }
            if (next_positional_ == FieldCount) {
                throw Error{"too many positional schema arguments"};
            }
            return assign(next_positional_++);
        }

        named_ = true;
        const auto index = std::forward<FindField>(find_field)(*name);
        if (!index) {
            throw Error{"a schema has no field with the supplied name"};
        }
        return assign(*index);
    }

    bool assigned(std::size_t index) const noexcept { return assigned_[index]; }

  private:
    std::size_t assign(std::size_t index) {
        if (assigned_[index]) {
            throw Error{"a schema field was supplied more than once"};
        }
        assigned_[index] = true;
        return index;
    }

    std::array<bool, FieldCount> assigned_{};
    std::size_t next_positional_{};
    bool named_{};
};

} // namespace bg::details
