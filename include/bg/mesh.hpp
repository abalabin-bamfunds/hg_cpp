#pragma once

#include <bg/export.hpp>
#include <bg/graph/graph_type.hpp>
#include <bg/mesh/details/mesh_invoke.hpp>
#include <bg/operator.hpp>
#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>

namespace bg {

namespace details {

template <template <typename> typename Field>
using MESH_POSITIONAL = BundleSchemaVariableDeclaration<"MESH_POSITIONAL", Field>;

template <template <typename> typename Field>
using MESH_NAMED = BundleSchemaVariableDeclaration<"MESH_NAMED", Field>;

} // namespace details

extern BG_EXPORT operator_<
    auto(GRAPH_T, TSB<details::MESH_POSITIONAL>, TSB<details::MESH_NAMED>) -> TS_T,
    details::mesh_operator_implementation>
    mesh;

} // namespace bg
