#pragma once

#include <hg/export.hpp>
#include <hg/graph/graph_type.hpp>
#include <hg/operator.hpp>
#include <hg/switch_argument.hpp>
#include <hg/switch/details/switch_invoke.hpp>
#include <hg/time_series/schema_variable.hpp>
#include <hg/time_series/tsb/tsb_type.hpp>

namespace hg {

namespace details {

template <template <typename, auto...> typename Field>
using SWITCH_POSITIONAL = BundleSchemaVariableDeclaration<"SWITCH_POSITIONAL", Field>;

template <template <typename, auto...> typename Field>
using SWITCH_NAMED = BundleSchemaVariableDeclaration<"SWITCH_NAMED", Field>;

} // namespace details

extern HG_EXPORT operator_<
    auto(TS<SCALAR>, GRAPH_T, TSB<details::SWITCH_POSITIONAL>,
         TSB<details::SWITCH_NAMED>) -> TS_T,
    details::switch_operator_implementation>
    switch_;

} // namespace hg
