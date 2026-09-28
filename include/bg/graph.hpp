#pragma once

#include <bg/graph/context.hpp>
#include <bg/graph/details/graph_factory.hpp>
#include <bg/graph/graph_type.hpp>
#include <bg/type/type_parameter.hpp>

#include <utility>

namespace bg {

template <typename Definition>
concept AggregateGraphClass = details::aggregate_graph_definition<Definition>::value;

namespace details {

template <typename... Declaration> struct aggregate_graph_declaration_parts {
    using Parsed = type_parameterized_declaration<Declaration...>;
    using Types = typename Parsed::Types;
    using Definition = typename Parsed::Definition;

    static constexpr auto valid = [] {
        if constexpr (type_parameter_declaration<Definition>) {
            return false;
        } else {
            return AggregateGraphClass<Definition> && Parsed::has_valid_parameters;
        }
    }();
};

template <typename... Declaration, typename... Arguments, std::size_t... Index>
[[nodiscard]] auto make_declared_aggregate_graph(std::index_sequence<Index...>, Arguments &&...arguments) {
    using Parts = aggregate_graph_declaration_parts<Declaration...>;
    return make_aggregate_graph<typename Parts::Definition, std::tuple_element_t<Index, typename Parts::Types>...>(
        std::forward<Arguments>(arguments)...);
}

} // namespace details

template <typename... TypeParameters, typename... Arguments>
    requires((details::type_parameter_declaration<TypeParameters> && ...) &&
             details::unnamed_argument_count<Arguments...> == 1 &&
             details::named_argument_count<"name", Arguments...> == 1)
[[nodiscard]] auto graph(Arguments &&...arguments) {
    auto &&function = details::unnamed_argument(std::forward<Arguments>(arguments)...);

    return details::make_callable_graph<TypeParameters...>(std::forward<decltype(function)>(function),
                                                           std::forward<Arguments>(arguments)...);
}

template <typename... Declaration, typename... Arguments>
    requires(sizeof...(Declaration) != 0 && details::aggregate_graph_declaration_parts<Declaration...>::valid)
[[nodiscard]] auto graph(Arguments &&...arguments) {
    return details::make_declared_aggregate_graph<Declaration...>(
        std::make_index_sequence<sizeof...(Declaration) - 1>{}, std::forward<Arguments>(arguments)...);
}

} // namespace bg
