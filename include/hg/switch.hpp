#pragma once

#include <bg/export.hpp>
#include <bg/graph/graph_type.hpp>
#include <bg/operator.hpp>
#include <bg/switch_argument.hpp>
#include <bg/switch/details/switch_invoke.hpp>
#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>

namespace bg {

namespace details {

template <template <typename, auto...> typename Field>
using SWITCH_POSITIONAL = BundleSchemaVariableDeclaration<"SWITCH_POSITIONAL", Field>;

template <template <typename, auto...> typename Field>
using SWITCH_NAMED = BundleSchemaVariableDeclaration<"SWITCH_NAMED", Field>;

} // namespace details

extern BG_EXPORT operator_<
    auto(TS<SCALAR>, GRAPH_T, TSB<details::SWITCH_POSITIONAL>,
         TSB<details::SWITCH_NAMED>) -> TS_T,
    details::switch_operator_implementation>
    switch_;

} // namespace bg
