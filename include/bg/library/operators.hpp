#pragma once

#include <bg/library/export.hpp>

#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/operators.hpp>
#include <bg/switch.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/type_variable.hpp>

namespace bg::operators {

extern BG_LIBRARY_EXPORT operator_<
    auto(Union<"lhs", SCALAR, TS<SCALAR>>, Union<"rhs", SCALAR, TS<SCALAR>>) -> TS<SCALAR>>
    add;

using ::bg::map;
using ::bg::mesh;
using ::bg::switch_;

} // namespace bg::operators
