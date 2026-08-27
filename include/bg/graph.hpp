#pragma once

#include <bg/graph/details/graph_factory.hpp>

#include <utility>

namespace bg {

template <typename Definition>
concept AggregateGraphClass = details::aggregate_graph_definition<Definition>::value;

template <typename... Arguments>
    requires(details::unnamed_argument_count<Arguments...> == 1 &&
             details::named_argument_count<"name", Arguments...> == 1)
[[nodiscard]] auto graph(Arguments &&...arguments) {
    auto &&function = details::unnamed_argument(std::forward<Arguments>(arguments)...);

    return details::make_callable_graph(std::forward<decltype(function)>(function),
                                        std::forward<Arguments>(arguments)...);
}

template <AggregateGraphClass Definition, typename... Arguments> [[nodiscard]] auto graph(Arguments &&...arguments) {
    return details::make_aggregate_graph<Definition>(std::forward<Arguments>(arguments)...);
}

} // namespace bg
