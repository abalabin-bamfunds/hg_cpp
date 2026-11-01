#pragma once

#include <hg/export.hpp>
#include <hg/graph/graph_type.hpp>
#include <hg/map_argument.hpp>
#include <hg/map/details/map_invoke.hpp>
#include <hg/operator.hpp>
#include <hg/time_series/schema_variable.hpp>
#include <hg/time_series/tsb/tsb_type.hpp>

namespace hg {

namespace details {

template <template <typename, auto...> typename Field>
using MAP_POSITIONAL = BundleSchemaVariableDeclaration<"MAP_POSITIONAL", Field>;

template <template <typename, auto...> typename Field>
using MAP_NAMED = BundleSchemaVariableDeclaration<"MAP_NAMED", Field>;

} // namespace details

extern HG_EXPORT operator_<
    auto(GRAPH_T, TSB<details::MAP_POSITIONAL>, TSB<details::MAP_NAMED>) -> TS_T,
    details::map_operator_implementation>
    map;

} // namespace hg
