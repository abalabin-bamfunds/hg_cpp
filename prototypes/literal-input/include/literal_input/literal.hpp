#pragma once

#include <ddv/serial_visitor.h>

#include <concepts>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace literal_input {

template <typename Leaf> class Literal {
  public:
    using sequence_type = std::span<const Literal>;

    Literal() noexcept = default;

    Literal(std::initializer_list<Literal> elements) noexcept
        : value_{std::in_place_index<1>, elements.begin(), elements.size()} {}

    template <typename Source>
        requires std::constructible_from<Leaf, Source &&> &&
                 (!std::same_as<std::remove_cvref_t<Source>, Literal>)
    Literal(Source &&source) : value_{std::in_place_index<0>, std::forward<Source>(source)} {}

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    decltype(auto) visit(Visitor &&visitor, Arguments &&...arguments) const {
        return std::forward<Visitor>(visitor).visit(value_, std::forward<Arguments>(arguments)...);
    }

    template <typename Function, typename... Functions>
    decltype(auto) visit(Function &&function, Functions &&...functions) const {
        return visit(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

  private:
    std::variant<Leaf, sequence_type> value_{std::in_place_index<1>};
};

} // namespace literal_input
