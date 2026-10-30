#pragma once

#include <bg/library/export.hpp>

#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/scalar/list/type.hpp>
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
extern BG_LIBRARY_EXPORT operator_<OUT, auto(TS_T) -> OUT> combine;
extern BG_LIBRARY_EXPORT operator_<OUT, auto(TS_T, Union<"reset", SIGNAL>) -> OUT> collect;
extern BG_LIBRARY_EXPORT operator_<OUT, auto(TS_T) -> OUT> emit;

// Text.
extern BG_LIBRARY_EXPORT operator_<auto(TEXT, i64, i64) -> string> substr;
extern BG_LIBRARY_EXPORT operator_<auto(TEXT) -> string> lower;
extern BG_LIBRARY_EXPORT operator_<auto(TEXT) -> string> upper;
extern BG_LIBRARY_EXPORT operator_<auto(TEXT) -> string> casefold;
extern BG_LIBRARY_EXPORT operator_<auto(SCALAR) -> string> str_;
extern BG_LIBRARY_EXPORT operator_<auto(string) -> string> format_;
extern BG_LIBRARY_EXPORT operator_<auto(string) -> void> print_;
extern BG_LIBRARY_EXPORT operator_<auto(string) -> void> log_;
extern BG_LIBRARY_EXPORT operator_<auto(REGEX_PATTERN, REGEX_TEXT) -> bool> matches;
extern BG_LIBRARY_EXPORT operator_<auto(REGEX_PATTERN, REGEX_REPLACEMENT, REGEX_TEXT) -> string> replace;
extern BG_LIBRARY_EXPORT operator_<auto(REGEX_PATTERN, REGEX_TEXT) -> OUT> match_;
extern BG_LIBRARY_EXPORT operator_<OUT, auto(TEXT, STRING_SEPARATOR, i64) -> OUT> split;
extern BG_LIBRARY_EXPORT operator_<auto(COLLECTION, STRING_SEPARATOR) -> string> join;

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
extern BG_LIBRARY_EXPORT operator_<auto(TS<SCALAR>) -> const Type *> type_;

// Tick selection.
extern BG_LIBRARY_EXPORT operator_<auto(SIGNAL, TS_T) -> TS_T> sample;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> TS_T> dedup;
extern BG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS_T) -> TSB<if_result>> if_;
extern BG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS_T) -> TS_T> filter_;
extern BG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS_T, i64) -> TS_T> gate;
extern BG_LIBRARY_EXPORT operator_<auto(TS<bool>, TS<SCALAR>, timedelta_t, i64) -> TS<List<SCALAR>>> batch;
extern BG_LIBRARY_EXPORT operator_<auto(TSL<TS_T, SIZE>) -> TS_T> merge;
extern BG_LIBRARY_EXPORT operator_<auto(TSL<TS_T, SIZE>) -> REF<TS_T>> race;
extern BG_LIBRARY_EXPORT operator_<OUT, auto(TS<i64>, TS_T) -> OUT> route_by_index;
extern BG_LIBRARY_EXPORT operator_<auto(TS<bool>, bool) -> TS<bool>> if_true;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T, i64) -> OUT> take;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T, i64) -> OUT> drop;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T, i64) -> OUT> step;
extern BG_LIBRARY_EXPORT operator_<auto(TS<bool>, TRUE_TS, FALSE_TS) -> OUT> if_then_else;

// Counting and lag.
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> i64> count;
extern BG_LIBRARY_EXPORT operator_<auto(TS<SCALAR>) -> TS<SCALAR>> lag;

// Analytical operators.
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> diff;
extern BG_LIBRARY_EXPORT operator_<auto(NUMERIC, NUMERIC, NUMERIC) -> NUMERIC> clip;
extern BG_LIBRARY_EXPORT operator_<auto(TS<double>, double, i64) -> OUT> ewma;

// Time observations.
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> date_time_t> last_modified_time;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> timedelta_t> elapsed;

// Timed operators.
extern BG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t) -> OUT> delay;
extern BG_LIBRARY_EXPORT operator_<auto(timedelta_t, bool, i64) -> TS<bool>> schedule;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t, bool) -> OUT> throttle;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t) -> OUT> debounce;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t) -> TS<bool>> timeout;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T, timedelta_t) -> OUT> resample;

// Running numeric aggregates. A valid reset-input tick clears the current aggregate.
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> sums;
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> mins;
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> maxs;
extern BG_LIBRARY_EXPORT operator_<auto(TS<NUMERIC>) -> OUT> means;

// Collection queries.
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> i64> len_;
extern BG_LIBRARY_EXPORT
    operator_<auto(COLLECTION, Union<"key", EQUALITY_SCALAR, TS<EQUALITY_SCALAR>>) -> bool> contains_;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> TS<bool>> is_empty;
extern BG_LIBRARY_EXPORT operator_<auto(TSD<K, TS_T>) -> TSS<K>> keys_;
extern BG_LIBRARY_EXPORT operator_<auto(TSD<K, TS<bool>>) -> TSS<K>> keys_where_true;
extern BG_LIBRARY_EXPORT operator_<auto(TSS<K>, TSS<K>) -> TSS<K>> bit_or;
extern BG_LIBRARY_EXPORT operator_<auto(TSS<K>, TSS<K>) -> TSS<K>> bit_and;
extern BG_LIBRARY_EXPORT operator_<auto(TSS<K>, TSS<K>) -> TSS<K>> bit_xor;
extern BG_LIBRARY_EXPORT operator_<auto(TSD<K, TS_T>, TSD<K, TS<K1>>) -> TSD<K1, REF<TS_T>>> rekey;
extern BG_LIBRARY_EXPORT
    operator_<auto(TSD<K, TS_T>, TSD<K, TS<K1>>, bool) -> TSD<K1, TSD<K, REF<TS_T>>>> partition;
extern BG_LIBRARY_EXPORT operator_<auto(TSD<K1, TSD<K, TS_T>>) -> TSD<K, REF<TS_T>>> unpartition;
extern BG_LIBRARY_EXPORT operator_<auto(TSD<K, TS<K1>>, bool) -> TSD<K1, TS<K>>> flip;
extern BG_LIBRARY_EXPORT
    operator_<auto(TSD<K, TSD<K1, TS_T>>, bool) -> TSD<K1, TSD<K, REF<TS_T>>>> flip_keys;
extern BG_LIBRARY_EXPORT operator_<auto(TSD<K, TSD<K1, TS_T>>) -> TSD<Tuple<K, K1>, REF<TS_T>>> collapse_keys;
extern BG_LIBRARY_EXPORT
    operator_<auto(TSD<Tuple<K, K1>, TS_T>, bool) -> TSD<K, TSD<K1, REF<TS_T>>>> uncollapse_keys;

// Collection reductions.
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> OUT> sum_;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> OUT> min_;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> OUT> max_;
extern BG_LIBRARY_EXPORT operator_<auto(TS_T) -> OUT> mean;
extern BG_LIBRARY_EXPORT operator_<auto(COLLECTION) -> OUT> all_;
extern BG_LIBRARY_EXPORT operator_<auto(COLLECTION) -> OUT> any_;

// Nested graph operators.
using ::bg::map;
using ::bg::mesh;
using ::bg::switch_;

} // namespace bg::operators
