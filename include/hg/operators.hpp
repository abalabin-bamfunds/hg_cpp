#pragma once

#include <hg/export.hpp>
#include <hg/operator.hpp>
#include <hg/scalar/declaration.hpp>
#include <hg/time_series/time_series_type.hpp>
#include <hg/time_series/time_series_variable.hpp>
#include <hg/type/scalar_variable.hpp>
#include <hg/type/type_variable.hpp>
#include <hg/util/symbol.hpp>

namespace hg::operators {

using COLLECTION = TimeSeriesVariable<"COLLECTION">;
using ITEM = TimeSeriesVariable<"ITEM">;
using CONVERT_RESULT = TimeSeriesVariable<"CONVERT_RESULT">;
using VALUE = ScalarVariable<"VALUE">;
using K = ScalarVariable<"K", Hashable>;

extern HG_EXPORT operator_<auto(SCALAR)->TS<SCALAR, TimeSeriesGuarantee::const_>> const_;
extern HG_EXPORT operator_<auto(TS<SCALAR>, SCALAR)->TS<SCALAR>> default_;
extern HG_EXPORT operator_<CONVERT_RESULT, auto(TS_T) -> CONVERT_RESULT> convert;
extern HG_EXPORT operator_<TS_T, auto() -> TS_T::with<TimeSeriesGuarantee::final>> nothing;
extern HG_EXPORT operator_<VALUE, auto(COLLECTION, Union<"key", K, TS<K>, TSS<K>>)->ITEM> getitem;
extern HG_EXPORT operator_<VALUE, auto(TS_T, SCALAR)->ITEM> getattr;

} // namespace hg::operators

#include <hg/graph/details/wiring_point_operators.hpp>
