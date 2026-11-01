#pragma once

#include <hg/export.hpp>
#include <hg/graph/graph_type.hpp>
#include <hg/mesh/details/mesh_invoke.hpp>
#include <hg/operator.hpp>
#include <hg/time_series/schema_variable.hpp>
#include <hg/time_series/tsb/tsb_type.hpp>

namespace hg {

namespace details {

template <template <typename, auto...> typename Field>
using MESH_POSITIONAL = BundleSchemaVariableDeclaration<"MESH_POSITIONAL", Field>;

template <template <typename, auto...> typename Field>
using MESH_NAMED = BundleSchemaVariableDeclaration<"MESH_NAMED", Field>;

} // namespace details

extern HG_EXPORT operator_<
    auto(GRAPH_T, TSB<details::MESH_POSITIONAL>, TSB<details::MESH_NAMED>) -> TS_T,
    details::mesh_operator_implementation>
    mesh;

} // namespace hg
