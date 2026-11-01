#pragma once

#include <hg/library/export.hpp>

#include <hg/map.hpp>
#include <hg/mesh.hpp>
#include <hg/operators.hpp>
#include <hg/scalar/builtins.hpp>
#include <hg/scalar/list/type.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/time_series_type.hpp>
#include <hg/time_series/time_series_variable.hpp>
#include <hg/type/scalar_variable.hpp>
#include <hg/type/type_variable.hpp>

namespace hg::operators {

using EQUALITY_SCALAR = ScalarVariable<"EQUALITY_SCALAR", EqualityComparable>;
using ORDERED_SCALAR = ScalarVariable<"ORDERED_SCALAR", i64, double, string, date_time_t, timedelta_t>;
using BOOL = ScalarVariable<"BOOL", bool>;
using NUMERIC = ScalarVariable<"NUMERIC", i64, double>;
using TEXT = ScalarVariable<"TEXT", string, symbol>;
using REGEX_PATTERN = ScalarVariable<"REGEX_PATTERN", string, symbol>;
using REGEX_REPLACEMENT = ScalarVariable<"REGEX_REPLACEMENT", string, symbol>;
using REGEX_TEXT = ScalarVariable<"REGEX_TEXT", string, symbol>;
using STRING_SEPARATOR = ScalarVariable<"STRING_SEPARATOR", string, symbol>;
template <template <typename, auto...> typename Field>
using FORMAT_POSITIONAL = BundleSchemaVariableDeclaration<"FORMAT_POSITIONAL", Field>;
template <template <typename, auto...> typename Field>
using FORMAT_NAMED = BundleSchemaVariableDeclaration<"FORMAT_NAMED", Field>;

using CONVERT_RESULT = TimeSeriesVariable<"CONVERT_RESULT">;
using SIGNAL = TimeSeriesVariable<"SIGNAL">;
using TRUE_TS = TimeSeriesVariable<"TRUE_TS">;
using FALSE_TS = TimeSeriesVariable<"FALSE_TS">;
using K1 = ScalarVariable<"K1", Hashable>;

template <template <typename, auto...> typename Field> struct if_result {
    Field<REF<TS_T>> true_value;
    Field<REF<TS_T>> false_value;
};

// Arithmetic and unary operators.
extern HG_LIBRARY_EXPORT operator_<auto(SCALAR, SCALAR) -> SCALAR> add;
extern HG_LIBRARY_EXPORT operator_<auto(SCALAR, SCALAR) -> SCALAR> sub;
extern HG_LIBRARY_EXPORT operator_<auto(SCALAR, SCALAR) -> SCALAR> mul;
extern HG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC) -> double> div;
extern HG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC) -> NUMERIC> floordiv;
extern HG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC) -> NUMERIC> mod;
extern HG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC) -> NUMERIC> pow;
extern HG_LIBRARY_EXPORT operator_<auto(double, i64) -> double> round;
extern HG_LIBRARY_EXPORT operator_<auto(SCALAR) -> SCALAR> pos;
extern HG_LIBRARY_EXPORT operator_<auto(SCALAR) -> SCALAR> neg;
extern HG_LIBRARY_EXPORT operator_<auto(SCALAR) -> SCALAR> abs;
extern HG_LIBRARY_EXPORT operator_<auto(SCALAR) -> SCALAR> invert;

// Conversion.
extern HG_LIBRARY_EXPORT operator_<CONVERT_RESULT, auto(TS_T) -> CONVERT_RESULT> convert;
extern HG_LIBRARY_EXPORT operator_<OUT, auto(TS_T) -> OUT> combine;
extern HG_LIBRARY_EXPORT operator_<OUT, auto(TS_T, Union<"reset", SIGNAL>) -> OUT> collect;
extern HG_LIBRARY_EXPORT operator_<OUT, auto(TS_T) -> OUT> emit;

// Text.
extern HG_LIBRARY_EXPORT operator_<auto(TEXT, i64, i64) -> string> substr;
extern HG_LIBRARY_EXPORT operator_<auto(TEXT) -> string> lower;
extern HG_LIBRARY_EXPORT operator_<auto(TEXT) -> string> upper;
extern HG_LIBRARY_EXPORT operator_<auto(TEXT) -> string> casefold;
extern HG_LIBRARY_EXPORT operator_<auto(SCALAR) -> string> str_;
extern HG_LIBRARY_EXPORT operator_<auto(string) -> string> format_;
extern HG_LIBRARY_EXPORT operator_<auto(string) -> void> print_;
extern HG_LIBRARY_EXPORT operator_<auto(string) -> void> log_;
extern HG_LIBRARY_EXPORT operator_<auto(REGEX_PATTERN, REGEX_TEXT) -> bool> matches;
extern HG_LIBRARY_EXPORT operator_<auto(REGEX_PATTERN, REGEX_REPLACEMENT, REGEX_TEXT) -> string> replace;
extern HG_LIBRARY_EXPORT operator_<auto(REGEX_PATTERN, REGEX_TEXT) -> OUT> match_;
extern HG_LIBRARY_EXPORT operator_<OUT, auto(TEXT, STRING_SEPARATOR, i64) -> OUT> split;
extern HG_LIBRARY_EXPORT operator_<auto(COLLECTION, STRING_SEPARATOR) -> string> join;

// Comparisons.
extern HG_LIBRARY_EXPORT operator_<EQUALITY_SCALAR, auto(EQUALITY_SCALAR, EQUALITY_SCALAR) -> bool> eq;
extern HG_LIBRARY_EXPORT operator_<EQUALITY_SCALAR, auto(EQUALITY_SCALAR, EQUALITY_SCALAR) -> bool> ne;
extern HG_LIBRARY_EXPORT operator_<ORDERED_SCALAR, auto(ORDERED_SCALAR, ORDERED_SCALAR) -> bool> lt;
extern HG_LIBRARY_EXPORT operator_<ORDERED_SCALAR, auto(ORDERED_SCALAR, ORDERED_SCALAR) -> bool> le;
extern HG_LIBRARY_EXPORT operator_<ORDERED_SCALAR, auto(ORDERED_SCALAR, ORDERED_SCALAR) -> bool> gt;
extern HG_LIBRARY_EXPORT operator_<ORDERED_SCALAR, auto(ORDERED_SCALAR, ORDERED_SCALAR) -> bool> ge;

// Boolean operators.
extern HG_LIBRARY_EXPORT operator_<BOOL, auto(BOOL, BOOL) -> bool> and_;
extern HG_LIBRARY_EXPORT operator_<BOOL, auto(BOOL, BOOL) -> bool> or_;
extern HG_LIBRARY_EXPORT operator_<BOOL, auto(BOOL) -> bool> not_;

// Validity queries.
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> bool> valid;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> bool> all_valid;
extern HG_LIBRARY_EXPORT operator_<auto(TS<SCALAR>) -> const Type *> type_;

// Tick selection.
extern HG_LIBRARY_EXPORT operator_<auto(SIGNAL, TS_T) -> TS_T> sample;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> TS_T> dedup;
extern HG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS_T) -> TSB<if_result>> if_;
extern HG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS_T) -> TS_T> filter_;
extern HG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS_T, i64, Union<"reset", SIGNAL>) -> TS_T> gate;
extern HG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS<SCALAR>, timedelta_t, i64) -> TS<List<SCALAR>>> batch;
extern HG_LIBRARY_EXPORT operator_<auto(TSL<TS_T, SIZE>) -> TS_T> merge;
extern HG_LIBRARY_EXPORT operator_<auto(TSL<TS_T, SIZE>) -> REF<TS_T>> race;
extern HG_LIBRARY_EXPORT operator_<OUT, auto(TS<i64>, TS_T) -> OUT> route_by_index;
extern HG_LIBRARY_EXPORT operator_<auto(TS<bool>, bool) -> TS<bool>> if_true;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, Union<"n", i64, timedelta_t>) -> OUT> take;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, Union<"n", i64, timedelta_t>) -> OUT> drop;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, i64) -> OUT> step;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, i64, i64, i64) -> OUT> slice_;
extern HG_LIBRARY_EXPORT operator_<auto(TS<bool>, TRUE_TS, FALSE_TS) -> OUT> if_then_else;

// Counting and lag.
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> i64> count;
extern HG_LIBRARY_EXPORT operator_<auto(TS<SCALAR>, i64) -> TS<SCALAR>> lag;

// Analytical operators.
extern HG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> diff;
extern HG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC, NUMERIC) -> NUMERIC> clip;
extern HG_LIBRARY_EXPORT operator_<auto(TS<double>, double, i64) -> OUT> ewma;

// Time observations.
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> date_time_t> last_modified_time;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> timedelta_t> elapsed;

// Timed operators.
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t) -> OUT> delay;
extern HG_LIBRARY_EXPORT operator_<auto(timedelta_t, bool, i64) -> TS<bool>> schedule;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t, bool) -> OUT> throttle;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t) -> OUT> debounce;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t) -> TS<bool>> timeout;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t) -> OUT> resample;

// Running numeric aggregates. A valid reset-input tick clears the current aggregate.
extern HG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> sums;
extern HG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> mins;
extern HG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> maxs;
extern HG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> means;

// Collection queries.
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> i64> len_;
extern HG_LIBRARY_EXPORT
    operator_<auto(COLLECTION, Union<"key", EQUALITY_SCALAR, TS<EQUALITY_SCALAR>>) -> bool> contains_;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> TS<bool>> is_empty;
extern HG_LIBRARY_EXPORT operator_<auto(COLLECTION) -> TSS<K>> keys_;
extern HG_LIBRARY_EXPORT operator_<OUT, auto(COLLECTION) -> OUT> values_;
extern HG_LIBRARY_EXPORT
    operator_<auto(COLLECTION, Union<"item", EQUALITY_SCALAR, TS<EQUALITY_SCALAR>>) -> i64> index_of;
extern HG_LIBRARY_EXPORT operator_<auto(TSD<K, TS<bool>>) -> TSS<K>> keys_where_true;
extern HG_LIBRARY_EXPORT operator_<auto(TSS<K>, TSS<K>) -> TSS<K>> bit_or;
extern HG_LIBRARY_EXPORT operator_<auto(TSS<K>, TSS<K>) -> TSS<K>> bit_and;
extern HG_LIBRARY_EXPORT operator_<auto(TSS<K>, TSS<K>) -> TSS<K>> bit_xor;
extern HG_LIBRARY_EXPORT operator_<auto(TSD<K, TS_T>, TSD<K, TS<K1>>) -> TSD<K1, REF<TS_T>>> rekey;
extern HG_LIBRARY_EXPORT
    operator_<auto(TSD<K, TS_T>, TSD<K, TS<K1>>, bool) -> TSD<K1, TSD<K, REF<TS_T>>>> partition;
extern HG_LIBRARY_EXPORT operator_<auto(TSD<K1, TSD<K, TS_T>>) -> TSD<K, REF<TS_T>>> unpartition;
extern HG_LIBRARY_EXPORT operator_<auto(TSD<K, TS<K1>>, bool) -> TSD<K1, TS<K>>> flip;
extern HG_LIBRARY_EXPORT
    operator_<auto(TSD<K, TSD<K1, TS_T>>, bool) -> TSD<K1, TSD<K, REF<TS_T>>>> flip_keys;
extern HG_LIBRARY_EXPORT operator_<auto(TSD<K, TSD<K1, TS_T>>) -> TSD<Tuple<K, K1>, REF<TS_T>>> collapse_keys;
extern HG_LIBRARY_EXPORT
    operator_<auto(TSD<Tuple<K, K1>, TS_T>, bool) -> TSD<K, TSD<K1, REF<TS_T>>>> uncollapse_keys;

// Collection reductions.
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> OUT> sum_;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> OUT> min_;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> OUT> max_;
extern HG_LIBRARY_EXPORT operator_<auto(TS_T) -> OUT> mean;
extern HG_LIBRARY_EXPORT operator_<auto(COLLECTION) -> OUT> all_;
extern HG_LIBRARY_EXPORT operator_<auto(COLLECTION) -> OUT> any_;

// Nested graph operators.
using ::hg::map;
using ::hg::mesh;
using ::hg::switch_;

} // namespace hg::operators
