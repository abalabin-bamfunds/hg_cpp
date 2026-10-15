#pragma once

#include <ddv/serial_visitor.h>

#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace literal {

template <typename Leaf> class Literal {
  public:
    using leaf_type = Leaf;
    using sequence_type = std::span<const Literal>;

    Literal() noexcept = default;

    Literal(std::initializer_list<Literal> elements) noexcept
        : value_{std::in_place_index<1>, SequenceStorage{elements.begin(), elements.size()}} {}

    template <typename Source>
        requires (!std::same_as<std::remove_cvref_t<Source>, Literal>) && std::constructible_from<Leaf, Source &&>
    Literal(Source &&source) : value_{std::in_place_index<0>, std::forward<Source>(source)} {}

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    decltype(auto) visit(Visitor &&visitor, Arguments &&...arguments) const {
        return std::visit(
            [&visitor, &arguments...](const auto &value) -> decltype(auto) {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, SequenceStorage>) {
                    return std::forward<Visitor>(visitor).visit(
                        sequence_type{value.data, value.size}, std::forward<Arguments>(arguments)...);
                } else {
                    return std::forward<Visitor>(visitor).visit(value, std::forward<Arguments>(arguments)...);
                }
            },
            value_);
    }

    template <typename Function, typename... Functions>
    decltype(auto) visit(Function &&function, Functions &&...functions) const {
        return visit(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

  private:
    struct SequenceStorage {
        const Literal *data;
        std::size_t size;
    };

    std::variant<Leaf, SequenceStorage> value_{std::in_place_index<1>};
};

} // namespace literal
