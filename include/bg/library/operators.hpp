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

using EQUALITY_SCALAR = ScalarVariable<"EQUALITY_SCALAR", EqualityComparable>;
using ORDERED_SCALAR = ScalarVariable<"ORDERED_SCALAR", i64, double, string, date_time_t, timedelta_t>;
using BOOL = ScalarVariable<"BOOL", bool>;
using NUMERIC = ScalarVariable<"NUMERIC", i64, double>;

using CONVERT_RESULT = TimeSeriesVariable<"CONVERT_RESULT">;
using SIGNAL = TimeSeriesVariable<"SIGNAL">;
using TRUE_TS = TimeSeriesVariable<"TRUE_TS">;
using FALSE_TS = TimeSeriesVariable<"FALSE_TS">;

// Arithmetic and unary operators.
extern BG_LIBRARY_EXPORT operator_<auto(SCALAR, SCALAR) -> SCALAR> add;
extern BG_LIBRARY_EXPORT operator_<auto(SCALAR, SCALAR) -> SCALAR> sub;
extern BG_LIBRARY_EXPORT operator_<auto(SCALAR, SCALAR) -> SCALAR> mul;
extern BG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC) -> double> div;
extern BG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC) -> NUMERIC> floordiv;
extern BG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC) -> NUMERIC> mod;
extern BG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC) -> NUMERIC> pow;
extern BG_LIBRARY_EXPORT operator_<auto(double, i64) -> double> round;
extern BG_LIBRARY_EXPORT operator_<auto(SCALAR) -> SCALAR> pos;
extern BG_LIBRARY_EXPORT operator_<auto(SCALAR) -> SCALAR> neg;
extern BG_LIBRARY_EXPORT operator_<auto(SCALAR) -> SCALAR> abs;

// Conversion.
extern BG_LIBRARY_EXPORT operator_<CONVERT_RESULT, auto(TS_T) -> CONVERT_RESULT> convert;

// Comparisons.
extern BG_LIBRARY_EXPORT operator_<EQUALITY_SCALAR, auto(EQUALITY_SCALAR, EQUALITY_SCALAR) -> bool> eq;
extern BG_LIBRARY_EXPORT operator_<EQUALITY_SCALAR, auto(EQUALITY_SCALAR, EQUALITY_SCALAR) -> bool> ne;
extern BG_LIBRARY_EXPORT operator_<ORDERED_SCALAR, auto(ORDERED_SCALAR, ORDERED_SCALAR) -> bool> lt;
extern BG_LIBRARY_EXPORT operator_<ORDERED_SCALAR, auto(ORDERED_SCALAR, ORDERED_SCALAR) -> bool> le;
extern BG_LIBRARY_EXPORT operator_<ORDERED_SCALAR, auto(ORDERED_SCALAR, ORDERED_SCALAR) -> bool> gt;
extern BG_LIBRARY_EXPORT operator_<ORDERED_SCALAR, auto(ORDERED_SCALAR, ORDERED_SCALAR) -> bool> ge;

// Boolean operators.
extern BG_LIBRARY_EXPORT operator_<BOOL, auto(BOOL, BOOL) -> bool> and_;
extern BG_LIBRARY_EXPORT operator_<BOOL, auto(BOOL, BOOL) -> bool> or_;
extern BG_LIBRARY_EXPORT operator_<BOOL, auto(BOOL) -> bool> not_;

// Validity queries.
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> bool> valid;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> bool> all_valid;

// Tick selection.
extern BG_LIBRARY_EXPORT operator_<auto(SIGNAL, TS_T) -> TS_T> sample;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> TS_T> dedup;
extern BG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS_T) -> TS_T> filter_;
extern BG_LIBRARY_EXPORT operator_<auto(TS<bool>, TRUE_TS, FALSE_TS) -> OUT> if_then_else;

// Counting and lag.
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> i64> count;
extern BG_LIBRARY_EXPORT operator_<auto(TS<SCALAR>) -> TS<SCALAR>> lag;

// Running numeric aggregates. A valid reset-input tick clears the current aggregate.
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> sums;
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> mins;
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> maxs;
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> means;

// Collection queries.
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> i64> len_;
extern BG_LIBRARY_EXPORT
    operator_<auto(COLLECTION, Union<"key", EQUALITY_SCALAR, TS<EQUALITY_SCALAR>>) -> bool> contains_;

// Nested graph operators.
using ::bg::map;
using ::bg::mesh;
using ::bg::switch_;

} // namespace bg::operators
