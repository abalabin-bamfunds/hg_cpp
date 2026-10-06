#pragma once

#include <bg/export.hpp>
#include <bg/graph/graph_type.hpp>
#include <bg/operator.hpp>
#include <bg/reduce/details/reduce_invoke.hpp>
#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/scalar_variable.hpp>

namespace bg {

namespace details {

template <template <typename, auto...> typename Field>
using REDUCE_POSITIONAL = BundleSchemaVariableDeclaration<"REDUCE_POSITIONAL", Field>;

template <template <typename, auto...> typename Field>
using REDUCE_NAMED = BundleSchemaVariableDeclaration<"REDUCE_NAMED", Field>;

} // namespace details

extern BG_EXPORT operator_<
    auto(GRAPH_T, TSB<details::REDUCE_POSITIONAL>, TSB<details::REDUCE_NAMED>) -> TS_T,
    details::reduce_operator_implementation>
    reduce;

} // namespace bg
