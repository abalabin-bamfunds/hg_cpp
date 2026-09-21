#pragma once

#include <bg/export.hpp>
#include <bg/graph/graph_type.hpp>
#include <bg/map/details/map_invoke.hpp>
#include <bg/operator.hpp>
#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>

namespace bg {

namespace details {

template <template <typename> typename Field>
using MAP_POSITIONAL = BundleSchemaVariableDeclaration<"MAP_POSITIONAL", Field>;

template <template <typename> typename Field>
using MAP_NAMED = BundleSchemaVariableDeclaration<"MAP_NAMED", Field>;

} // namespace details

extern BG_EXPORT operator_<
    auto(GRAPH_T, TSB<details::MAP_POSITIONAL>, TSB<details::MAP_NAMED>) -> TS_T,
    details::map_operator_invocation>
    map;

} // namespace bg
