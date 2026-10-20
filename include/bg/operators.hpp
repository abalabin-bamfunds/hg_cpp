#pragma once

#include <bg/export.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/declaration.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/util/symbol.hpp>

namespace bg::operators {

using COLLECTION = TimeSeriesVariable<"COLLECTION">;
using ITEM = TimeSeriesVariable<"ITEM">;
using VALUE = ScalarVariable<"VALUE">;
using K = ScalarVariable<"K", Hashable>;

extern BG_EXPORT operator_<auto(SCALAR)->TS<SCALAR, TimeSeriesGuarantee::const_>> const_;
extern BG_EXPORT operator_<auto(TS<SCALAR>, SCALAR)->TS<SCALAR>> default_;
extern BG_EXPORT operator_<VALUE, auto(COLLECTION, Union<"key", K, TS<K>>)->ITEM> getitem;
extern BG_EXPORT operator_<VALUE, auto(TS_T, SCALAR)->ITEM> getattr;

} // namespace bg::operators

#include <bg/graph/details/wiring_point_operators.hpp>
