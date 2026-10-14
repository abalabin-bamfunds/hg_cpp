#pragma once

#include <bg/graph/operator.hpp>
#include <bg/node.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace bg {

template <auto &OP, typename Definition = void>
    requires std::derived_from<std::remove_cvref_t<decltype(OP)>, Operator>
class overload final {
  public:
    overload()
        requires requires { node<Definition>(); }
    {
        OP.overload(node<Definition>());
    }
};

template <auto &OP>
class overload<OP, void> final {
  public:
    template <typename Implementation>
        requires std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate>
    explicit overload(Implementation &&implementation) {
        OP.overload(std::forward<Implementation>(implementation));
    }

    template <typename... Arguments>
        requires(sizeof...(Arguments) != 0 &&
                 (!std::derived_from<std::remove_cvref_t<Arguments>, WiringTemplate> && ...) &&
                 requires(Arguments &&...arguments) { node(std::forward<Arguments>(arguments)...); })
    explicit overload(Arguments &&...arguments) {
        OP.overload(node(std::forward<Arguments>(arguments)...));
    }
};

template <auto &OP, template <typename> typename DefinitionTemplate, typename... Types>
    requires std::derived_from<std::remove_cvref_t<decltype(OP)>, Operator> && (sizeof...(Types) != 0) &&
             (requires { node<DefinitionTemplate<Types>>(); } && ...)
class instantiate_overloads final {
  public:
    instantiate_overloads() {
        (OP.overload(node<DefinitionTemplate<Types>>()), ...);
    }
};

} // namespace bg
