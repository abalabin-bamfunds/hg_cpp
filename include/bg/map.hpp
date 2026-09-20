#pragma once

#include <bg/export.hpp>
#include <bg/map/details/map_invoke.hpp>
#include <bg/operator.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace bg {

class MapOperator : public Operator {
  public:
    MapOperator();

    using Operator::overload;

    template <typename Graph, typename... Arguments>
        requires std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate> &&
                 requires { typename std::remove_cvref_t<Graph>::result_type; }
    [[nodiscard]] auto operator()(const Graph &function, Arguments &&...arguments) {
        return details::invoke_map_graph(*this, function, std::forward<Arguments>(arguments)...);
    }

    template <typename Function, typename... Arguments>
        requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
    [[nodiscard]] auto operator()(Function &&function, Arguments &&...arguments) {
        return details::invoke_raw_map_graph(*this, std::forward<Function>(function),
                                             std::forward<Arguments>(arguments)...);
    }
};

extern BG_EXPORT MapOperator map;

} // namespace bg
