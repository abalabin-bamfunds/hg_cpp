#pragma once

#include <hg/export.hpp>
#include <hg/graph/graph_type.hpp>
#include <hg/operator.hpp>
#include <hg/reduce/details/reduce_invoke.hpp>
#include <hg/time_series/schema_variable.hpp>
#include <hg/time_series/tsb/tsb_type.hpp>
#include <hg/time_series/time_series_variable.hpp>
#include <hg/type/scalar_variable.hpp>

namespace hg {

namespace details {

template <template <typename, auto...> typename Field>
using REDUCE_POSITIONAL = BundleSchemaVariableDeclaration<"REDUCE_POSITIONAL", Field>;

template <template <typename, auto...> typename Field>
using REDUCE_NAMED = BundleSchemaVariableDeclaration<"REDUCE_NAMED", Field>;

} // namespace details

extern HG_EXPORT operator_<
    auto(GRAPH_T, TSB<details::REDUCE_POSITIONAL>, TSB<details::REDUCE_NAMED>) -> TS_T,
    details::reduce_operator_implementation>
    reduce;

} // namespace hg
