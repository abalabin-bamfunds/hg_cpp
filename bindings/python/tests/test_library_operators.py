import math
import random
import subprocess
import sys
from typing import Annotated

import pytest

import hgraph as hg


def test_add_operator_accepts_double_scalars_and_time_series():
    @hg.graph
    def scalar_add() -> hg.TS[float]:
        return hg.operators.add(1.25, 2.5)

    @hg.graph
    def mixed_add(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.add(value, 2.5)

    @hg.graph
    def reversed_mixed_add(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.add(2.5, value)

    @hg.graph
    def series_add(lhs: hg.TS[float], rhs: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.add(lhs, rhs)

    @hg.graph
    def keyword_add(lhs: hg.TS[float], rhs: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.add(rhs=rhs, lhs=lhs)

    @hg.graph
    def promoted_add(lhs: float, rhs: float) -> float:
        return hg.operators.add(lhs, rhs)

    assert hg.run_graph(scalar_add) == [3.75]
    assert hg.run_graph(mixed_add, [1.0, 2.0]) == [3.5, 4.5]
    assert hg.run_graph(reversed_mixed_add, [1.0, 2.0]) == [3.5, 4.5]
    assert hg.run_graph(series_add, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]
    assert hg.run_graph(keyword_add, [1.0], [10.0]) == [11.0]
    assert hg.run_graph(promoted_add, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]


def test_add_operator_accepts_i64_scalars_and_time_series():
    @hg.graph
    def scalar_add() -> hg.TS[int]:
        return hg.operators.add(2, 3)

    @hg.graph
    def series_plus_scalar(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.add(value, 3)

    @hg.graph
    def scalar_plus_series(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.add(3, value)

    assert hg.run_graph(scalar_add) == [5]
    assert hg.run_graph(series_plus_scalar, [1, -2]) == [4, 1]
    assert hg.run_graph(scalar_plus_series, [1, -2]) == [4, 1]


def test_i64_arithmetic_wraps_at_signed_boundaries():
    minimum = -(1 << 63)
    maximum = (1 << 63) - 1

    @hg.graph
    def add(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return lhs + rhs

    @hg.graph
    def subtract(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return lhs - rhs

    @hg.graph
    def multiply(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return lhs * rhs

    @hg.graph
    def negate(value: hg.TS[int]) -> hg.TS[int]:
        return -value

    @hg.graph
    def absolute(value: hg.TS[int]) -> hg.TS[int]:
        return abs(value)

    assert hg.run_graph(add, [maximum, minimum], [1, -1]) == [minimum, maximum]
    assert hg.run_graph(subtract, [minimum, maximum], [1, -1]) == [maximum, minimum]
    assert hg.run_graph(multiply, [maximum, minimum], [2, -1]) == [-2, minimum]
    assert hg.run_graph(negate, [minimum, maximum]) == [minimum, -maximum]
    assert hg.run_graph(absolute, [minimum, -maximum]) == [minimum, maximum]


def test_i64_bitwise_operators_accept_scalars_and_time_series():
    minimum = -(1 << 63)
    maximum = (1 << 63) - 1

    @hg.graph
    def scalar_and() -> hg.TS[int]:
        return hg.operators.bit_and(-1, 0b0101)

    @hg.graph
    def scalar_invert() -> hg.TS[int]:
        return hg.operators.invert(minimum)

    @hg.graph
    def bit_and(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return lhs & rhs

    @hg.graph
    def bit_or(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return lhs | rhs

    @hg.graph
    def bit_xor(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return lhs ^ rhs

    @hg.graph
    def invert(value: hg.TS[int]) -> hg.TS[int]:
        return ~value

    @hg.graph
    def scalar_left(value: hg.TS[int]) -> hg.TS[int]:
        return -1 & value

    @hg.graph
    def scalar_right(value: hg.TS[int]) -> hg.TS[int]:
        return value & -1

    @hg.graph
    def deduplicated_invert(value: hg.TS[int]) -> hg.TS[int]:
        return ~hg.operators.dedup(value)

    @hg.graph
    def ordinary_zero_and(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.bit_and(value, 0)

    @hg.graph
    def deduplicated_zero_and(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.bit_and(hg.operators.dedup(value), 0)

    lhs = [-1, minimum, maximum, 0b1010]
    rhs = [0b0101, -1, 1, 0b1100]

    assert hg.run_graph(scalar_and) == [0b0101]
    assert hg.run_graph(scalar_invert) == [maximum]
    assert hg.run_graph(bit_and, lhs, rhs) == [0b0101, minimum, 1, 0b1000]
    assert hg.run_graph(bit_or, lhs, rhs) == [-1, -1, maximum, 0b1110]
    assert hg.run_graph(bit_xor, lhs, rhs) == [-6, maximum, maximum - 1, 0b0110]
    assert hg.run_graph(invert, [-1, minimum, maximum, 0b1010]) == [0, maximum, minimum, -11]
    assert hg.run_graph(scalar_left, [0b0101, minimum]) == [0b0101, minimum]
    assert hg.run_graph(scalar_right, [0b0101, minimum]) == [0b0101, minimum]
    assert hg.run_graph(deduplicated_invert, [5, 5, 6]) == [-6, None, -7]
    assert hg.run_graph(ordinary_zero_and, [1, 2, 3]) == [0, 0, 0]
    assert hg.run_graph(deduplicated_zero_and, [1, 1, 2]) == [0, None, None]
    assert hg.run_graph(bit_and, [7, hg.INVALID, 8], [3, None, None]) == [3, None, 0]


def test_i64_bitwise_overloads_preserve_tss_set_syntax():
    @hg.graph
    def union(lhs: hg.TSS[int], rhs: hg.TSS[int]) -> hg.TSS[int]:
        return lhs | rhs

    @hg.graph
    def intersection(lhs: hg.TSS[int], rhs: hg.TSS[int]) -> hg.TSS[int]:
        return lhs & rhs

    @hg.graph
    def symmetric_difference(lhs: hg.TSS[int], rhs: hg.TSS[int]) -> hg.TSS[int]:
        return lhs ^ rhs

    lhs = [{1, 2}]
    rhs = [{2, 3}]

    assert hg.run_graph(union, lhs, rhs) == [hg.SetDelta(added={1, 2, 3})]
    assert hg.run_graph(intersection, lhs, rhs) == [hg.SetDelta(added={2})]
    assert hg.run_graph(symmetric_difference, lhs, rhs) == [hg.SetDelta(added={1, 3})]


def test_running_numeric_operators_and_named_reset_input():
    @hg.graph
    def sums(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.sums(value)

    @hg.graph
    def mins(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.mins(value)

    @hg.graph
    def maxs(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.maxs(value)

    @hg.graph
    def means(value: hg.TS[int]) -> hg.TS[float]:
        return hg.operators.means(value)

    @hg.graph
    def reset_sums(value: hg.TS[int], reset: hg.TS[bool]) -> hg.TS[int]:
        return hg.operators.sums(value, reset=reset)

    @hg.graph
    def reset_mins(value: hg.TS[int], reset: hg.TS[bool]) -> hg.TS[int]:
        return hg.operators.mins(value, reset=reset)

    @hg.graph
    def reset_maxs(value: hg.TS[int], reset: hg.TS[bool]) -> hg.TS[int]:
        return hg.operators.maxs(value, reset=reset)

    @hg.graph
    def reset_means(value: hg.TS[int], reset: hg.TS[bool]) -> hg.TS[float]:
        return hg.operators.means(value, reset=reset)

    assert hg.run_graph(sums, [1, 1, 2]) == [1, 2, 4]
    assert hg.run_graph(mins, [4.0, 4.0, 6.0, 3.0]) == [4.0, None, None, 3.0]
    assert hg.run_graph(maxs, [4.0, 4.0, 6.0, 3.0]) == [4.0, None, 6.0, None]
    assert hg.run_graph(means, [1, 2, 3]) == [1.0, 1.5, 2.0]
    values = [5, 4, 6, 8, 3, 9, None, 7]
    resets = [None, False, True, None, False, True, True, None]
    assert hg.run_graph(reset_sums, values, resets) == [5, 4, 6, 14, 3, 9, None, 7]
    assert hg.run_graph(reset_mins, values, resets) == [5, 4, 6, None, 3, 9, None, 7]
    assert hg.run_graph(reset_maxs, values, resets) == [5, 4, 6, 8, 3, 9, None, 7]
    assert hg.run_graph(reset_means, values, resets) == [5.0, 4.0, 6.0, 7.0, 3.0, 9.0, None, 7.0]


def test_running_numeric_operators_handle_input_invalidation_with_named_reset():
    @hg.graph
    def sums(value: hg.TS[int], reset: hg.TS[str]) -> hg.TS[int]:
        return hg.operators.sums(value, reset=reset)

    @hg.graph
    def mins(value: hg.TS[int], reset: hg.TS[str]) -> hg.TS[int]:
        return hg.operators.mins(value, reset=reset)

    @hg.graph
    def maxs(value: hg.TS[int], reset: hg.TS[str]) -> hg.TS[int]:
        return hg.operators.maxs(value, reset=reset)

    @hg.graph
    def means(value: hg.TS[int], reset: hg.TS[str]) -> hg.TS[float]:
        return hg.operators.means(value, reset=reset)

    values = [5, 4, hg.INVALID, 3, 2]
    resets = [None, None, None, None, None]
    assert hg.run_graph(sums, values, resets) == [5, 9, None, 3, 5]
    assert hg.run_graph(mins, values, resets) == [5, 4, None, 3, 2]
    assert hg.run_graph(maxs, values, resets) == [5, None, None, 3, None]
    assert hg.run_graph(means, values, resets) == [5.0, 4.5, None, 3.0, 2.5]


def test_running_numeric_operators_preserve_history_on_reset_invalidation():
    @hg.graph
    def sums(value: hg.TS[int], reset: hg.TS[str]) -> hg.TS[int]:
        return hg.operators.sums(value, reset=reset)

    @hg.graph
    def mins(value: hg.TS[int], reset: hg.TS[str]) -> hg.TS[int]:
        return hg.operators.mins(value, reset=reset)

    @hg.graph
    def maxs(value: hg.TS[int], reset: hg.TS[str]) -> hg.TS[int]:
        return hg.operators.maxs(value, reset=reset)

    @hg.graph
    def means(value: hg.TS[int], reset: hg.TS[str]) -> hg.TS[float]:
        return hg.operators.means(value, reset=reset)

    values = [5, 4, None, 3]
    resets = [None, "reset", hg.INVALID, None]
    assert hg.run_graph(sums, values, resets) == [5, 4, None, 7]
    assert hg.run_graph(mins, values, resets) == [5, 4, None, 3]
    assert hg.run_graph(maxs, values, resets) == [5, 4, None, None]
    assert hg.run_graph(means, values, resets) == [5.0, 4.0, None, 3.5]


def test_running_extrema_keep_initial_nan_until_input_invalidation():
    @hg.graph
    def mins(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.mins(value)

    @hg.graph
    def maxs(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.maxs(value)

    @hg.graph
    def mins_valid(value: hg.TS[float]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.mins(value))

    @hg.graph
    def maxs_valid(value: hg.TS[float]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.maxs(value))

    values = [math.nan, 1.0, math.nan, 0.0, hg.INVALID, None, 3.0]
    minimum = hg.run_graph(mins, values)
    maximum = hg.run_graph(maxs, values)
    assert math.isnan(minimum[0])
    assert math.isnan(maximum[0])
    assert minimum[1:] == [None, None, None, None, None, 3.0]
    assert maximum[1:] == [None, None, None, None, None, 3.0]
    assert hg.run_graph(mins_valid, values) == [True, None, None, None, False, None, True]
    assert hg.run_graph(maxs_valid, values) == [True, None, None, None, False, None, True]


def test_running_extrema_ignore_later_nan_without_invalidating_output():
    @hg.graph
    def mins(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.mins(value)

    @hg.graph
    def maxs(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.maxs(value)

    @hg.graph
    def mins_valid(value: hg.TS[float]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.mins(value))

    @hg.graph
    def maxs_valid(value: hg.TS[float]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.maxs(value))

    values = [2.0, math.nan, 3.0, 1.0]
    assert hg.run_graph(mins, values) == [2.0, None, None, 1.0]
    assert hg.run_graph(maxs, values) == [2.0, None, 3.0, None]
    assert hg.run_graph(mins_valid, values) == [True, None, None, None]
    assert hg.run_graph(maxs_valid, values) == [True, None, None, None]


def test_running_extrema_clear_nan_history_on_reset_only_and_simultaneous_ticks():
    @hg.graph
    def mins(value: hg.TS[float], reset: hg.TS[bool]) -> hg.TS[float]:
        return hg.operators.mins(value, reset=reset)

    @hg.graph
    def maxs(value: hg.TS[float], reset: hg.TS[bool]) -> hg.TS[float]:
        return hg.operators.maxs(value, reset=reset)

    @hg.graph
    def mins_valid(value: hg.TS[float], reset: hg.TS[bool]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.mins(value, reset=reset))

    @hg.graph
    def maxs_valid(value: hg.TS[float], reset: hg.TS[bool]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.maxs(value, reset=reset))

    values = [math.nan, 1.0, None, 4.0, math.nan, 5.0, 2.0, 3.0]
    resets = [None, None, True, None, False, None, True, None]
    minimum = hg.run_graph(mins, values, resets)
    maximum = hg.run_graph(maxs, values, resets)
    assert math.isnan(minimum[0])
    assert math.isnan(maximum[0])
    assert minimum[1:4] == [None, None, 4.0]
    assert maximum[1:4] == [None, None, 4.0]
    assert math.isnan(minimum[4])
    assert math.isnan(maximum[4])
    assert minimum[5:] == [None, 2.0, None]
    assert maximum[5:] == [None, 2.0, 3.0]
    expected_validity = [True, None, False, True, None, None, None, None]
    assert hg.run_graph(mins_valid, values, resets) == expected_validity
    assert hg.run_graph(maxs_valid, values, resets) == expected_validity


@pytest.mark.parametrize("operator_name", ["sums", "mins", "maxs", "means"])
@pytest.mark.parametrize("value_type", [bool, str])
def test_running_numeric_operators_reject_non_numeric_time_series(operator_name, value_type):
    operator = getattr(hg.operators, operator_name)

    @hg.graph
    def unsupported(value: hg.TS[value_type]) -> hg.TS[value_type]:
        return operator(value)

    with pytest.raises(ValueError, match=f"Cannot resolve operator {operator_name}"):
        hg.run_graph(unsupported, [True if value_type is bool else "value"])


def test_python_operator_syntax_and_named_boolean_operators():
    @hg.graph
    def arithmetic(lhs: hg.TS[float], rhs: hg.TS[float]) -> hg.TS[float]:
        return abs(-(lhs - rhs) * rhs)

    @hg.graph
    def comparison(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.and_(lhs < rhs, hg.operators.not_(rhs < 0))

    assert hg.run_graph(arithmetic, [5.0, 2.0], [2.0, 3.0]) == [6.0, 3.0]
    assert hg.run_graph(comparison, [1, 4, 3], [2, 3, -1]) == [True, False, False]


def test_collection_query_operators_accept_public_keyword_names():
    @hg.schema
    class Pair:
        left: hg.TS[int]
        right: hg.TS[float]

    @hg.graph
    def fixed_list_length(value: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return hg.operators.len_(value=value)

    @hg.graph
    def fixed_bundle_length(value: hg.TSB[Pair]) -> hg.TS[int]:
        return hg.operators.len_(value=value)

    @hg.graph
    def set_length(value: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.len_(value=value)

    @hg.graph
    def set_contains(collection: hg.TSS[int], key: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=key)

    @hg.graph
    def dictionary_length(value: hg.TS[dict[int, int]]) -> hg.TS[int]:
        return hg.operators.len_(value=value)

    @hg.graph
    def dictionary_contains(collection: hg.TS[dict[int, int]], key: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=key)

    @hg.graph
    def dictionary_contains_scalar(collection: hg.TS[dict[int, int]]) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=2)

    assert hg.run_graph(fixed_list_length, [{0: 10, 1: 20}]) == [2]
    assert hg.run_graph(fixed_bundle_length, [{"left": 10, "right": 20.0}]) == [2]
    assert hg.run_graph(set_length, [set(), {1, 2}, hg.SetDelta(added={3}, removed={1})]) == [0, 2, 2]
    assert hg.run_graph(set_contains, [{1}, {2, hg.Removed(1)}, {3, hg.Removed(2)}], [1, 1, 3]) == [True, False, True]
    assert hg.run_graph(dictionary_length, [{1: 10, 2: 20}, {}]) == [2, 0]
    assert hg.run_graph(dictionary_contains, [{1: 10, 2: 20}, {}, {3: 30}], [2, 1, 3]) == [True, False, True]
    assert hg.run_graph(dictionary_contains_scalar, [{1: 10, 2: 20}, {}, {3: 30}]) == [True, False, False]


def test_tsd_contains_operator_distinguishes_physical_and_valid_items_membership():
    @hg.graph
    def physical_contains(collection: hg.TSD[int, hg.TS[int]], key: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=key)

    @hg.graph
    def valid_items_contains(
        collection: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items], key: hg.TS[int]
    ) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=key)

    dictionary_updates = [{2: 20}, {2: hg.INVALID}, {2: 21}]
    keys = [2, 2, 2]
    assert hg.run_graph(physical_contains, dictionary_updates, keys) == [True, True, True]
    assert hg.run_graph(valid_items_contains, dictionary_updates, keys) == [True, False, True]

    @hg.graph
    def physical_scalar_contains(collection: hg.TSD[int, hg.TS[int]]) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=2)

    @hg.graph
    def valid_items_scalar_contains(
        collection: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items]
    ) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=2)

    unrelated_key_updates = [{2: 20}, {3: 30}, {3: 31}]
    assert hg.run_graph(physical_scalar_contains, unrelated_key_updates) == [True, None, None]
    assert hg.run_graph(valid_items_scalar_contains, unrelated_key_updates) == [True, None, None]


def test_tsd_contains_operator_follows_dynamic_keys_through_absent_key_churn():
    @hg.graph
    def physical_contains(collection: hg.TSD[int, hg.TS[int]], key: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=key)

    @hg.graph
    def valid_items_contains(
        collection: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items], key: hg.TS[int]
    ) -> hg.TS[bool]:
        return hg.operators.contains_(collection=collection, key=key)

    collection = [{2: 20, 3: 30}]
    keys = [2, 4, 5, 3, 4]
    expected = [True, False, False, True, False]
    assert hg.run_graph(physical_contains, collection, keys) == expected
    assert hg.run_graph(valid_items_contains, collection, keys) == expected


def test_tsd_collection_queries_accept_valid_items():
    @hg.graph
    def keys(values: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items]) -> hg.TSS[int]:
        return hg.operators.keys_(values)

    @hg.graph
    def length(values: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items]) -> hg.TS[int]:
        return hg.operators.len_(values)

    @hg.graph
    def empty(values: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items]) -> hg.TS[bool]:
        return hg.operators.is_empty(values)

    updates = [
        {1: 10, 2: hg.INVALID},
        {2: 20},
        {1: hg.INVALID},
        {1: hg.REMOVE},
        {3: 30},
    ]
    assert hg.run_graph(keys, updates) == [
        hg.SetDelta(added={1}), hg.SetDelta(added={2}), hg.SetDelta(removed={1}), None,
        hg.SetDelta(added={3})
    ]
    assert hg.run_graph(length, updates) == [1, 2, 1, None, 2]
    assert hg.run_graph(empty, updates) == [False, False, False, None, False]


def test_tsd_set_operators_compose_with_valid_items():
    @hg.graph
    def intersection(lhs: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items],
                     rhs: hg.TSD[int, hg.TS[int]]) -> hg.TSS[int]:
        return hg.operators.keys_(lhs & rhs)

    @hg.graph
    def difference(lhs: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items],
                   rhs: hg.TSD[int, hg.TS[int]]) -> hg.TSS[int]:
        return hg.operators.keys_(lhs - rhs)

    @hg.graph
    def union(lhs: hg.TSD[int, hg.TS[int]],
              rhs: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items]) -> hg.TSS[int]:
        return hg.operators.keys_(lhs | rhs)

    lhs = [
        {1: 10, 2: hg.INVALID, 3: 30},
        {2: 20},
        {1: hg.INVALID},
        {2: hg.REMOVE},
    ]
    rhs = [
        {2: 200, 3: 300},
        {1: 100},
        {3: hg.REMOVE},
        {5: 500},
    ]

    assert hg.run_graph(intersection, lhs, rhs) == [
        hg.SetDelta(added={3}), hg.SetDelta(added={1, 2}), hg.SetDelta(removed={1, 3}),
        hg.SetDelta(removed={2})
    ]
    assert hg.run_graph(difference, lhs, rhs) == [
        hg.SetDelta(added={1}), hg.SetDelta(removed={1}), hg.SetDelta(added={3}), None
    ]
    assert hg.run_graph(union, lhs, rhs) == [
        hg.SetDelta(added={1, 2, 3}), None, None, hg.SetDelta(added={5}),
    ]


@pytest.mark.parametrize(
    ("operation_name", "lhs_valid", "rhs_valid", "expected"),
    [
        ("intersection", False, False, {2, 3}),
        ("intersection", True, False, {3}),
        ("intersection", False, True, {2}),
        ("intersection", True, True, set()),
        ("difference", False, False, {1, 5}),
        ("difference", True, False, {1}),
        ("difference", False, True, {1, 3, 5}),
        ("difference", True, True, {1, 3}),
        ("union", False, False, {1, 2, 3, 4, 5}),
        ("union", True, False, {1, 2, 3, 4}),
        ("union", False, True, {1, 2, 3, 5}),
        ("union", True, True, {1, 2, 3}),
        ("symmetric_difference", False, False, {1, 4, 5}),
        ("symmetric_difference", True, False, {1, 2, 4}),
        ("symmetric_difference", False, True, {1, 3, 5}),
        ("symmetric_difference", True, True, {1, 2, 3}),
    ],
)
def test_tsd_set_operators_accept_valid_items_on_either_side(
    operation_name, lhs_valid, rhs_valid, expected
):
    dictionary_type = hg.TSD[int, hg.TS[int]]
    lhs_type = Annotated[dictionary_type, hg.valid_items] if lhs_valid else dictionary_type
    rhs_type = Annotated[dictionary_type, hg.valid_items] if rhs_valid else dictionary_type
    operations = {
        "intersection": lambda lhs, rhs: lhs & rhs,
        "difference": lambda lhs, rhs: lhs - rhs,
        "union": lambda lhs, rhs: lhs | rhs,
        "symmetric_difference": lambda lhs, rhs: lhs ^ rhs,
    }
    operation = operations[operation_name]

    @hg.graph
    def projected(lhs: lhs_type, rhs: rhs_type) -> hg.TSS[int]:
        return hg.operators.keys_(operation(lhs, rhs))

    lhs = [{1: 10, 2: hg.INVALID, 3: 30, 5: hg.INVALID}]
    rhs = [{2: 200, 3: hg.INVALID, 4: hg.INVALID}]
    assert hg.run_graph(projected, lhs, rhs) == [hg.SetDelta(added=expected)]


def test_tss_equality_and_is_empty():
    @hg.graph
    def equal(lhs: hg.TSS[int], rhs: hg.TSS[int]) -> hg.TS[bool]:
        return lhs == rhs

    @hg.graph
    def empty(values: hg.TSS[int]) -> hg.TS[bool]:
        return hg.operators.is_empty(value=values)

    assert hg.run_graph(equal, [{1}, {2, hg.Removed(1)}, {4}],
                         [{1}, {3, hg.Removed(1)}, {5}]) == [True, False, False]
    assert hg.run_graph(empty, [set(), {1}, hg.SetDelta(removed={1})]) == [True, False, True]


def test_optional_tss_operators_treat_invalid_sets_as_empty():
    @hg.graph
    def equal(condition: hg.TS[bool], values: hg.TSS[int], invalid: hg.TSS[int], empty: hg.TSS[int]) -> hg.TS[bool]:
        selected = hg.operators.if_then_else(condition, values, invalid)
        return hg.operators.eq(lhs=hg.opt(selected), rhs=hg.opt(empty))

    @hg.graph
    def empty(condition: hg.TS[bool], values: hg.TSS[int], invalid: hg.TSS[int]) -> hg.TS[bool]:
        selected = hg.operators.if_then_else(condition, values, invalid)
        return hg.operators.is_empty(value=hg.opt(selected))

    condition = [False, True, True, False]
    values = [None, set(), {1}, None]
    invalid = [None, None, None, None]
    assert hg.run_graph(equal, condition, values, invalid, [set(), None, None, None]) == [True, True, False, True]
    assert hg.run_graph(empty, condition, values, invalid) == [True, True, False, True]


def test_tss_set_operators_and_bitwise_syntax():
    @hg.graph
    def union(lhs: hg.TSS[int], rhs: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.sum_(lhs | rhs)

    @hg.graph
    def intersection(lhs: hg.TSS[int], rhs: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.sum_(lhs & rhs)

    @hg.graph
    def difference(lhs: hg.TSS[int], rhs: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.sum_(lhs - rhs)

    @hg.graph
    def symmetric_difference(lhs: hg.TSS[int], rhs: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.sum_(lhs ^ rhs)

    lhs = [
        {1, 2},
        hg.SetDelta(added={3}, removed={1}),
        None,
        hg.SetDelta(removed={2}),
    ]
    rhs = [
        {2, 3},
        hg.SetDelta(added={1}, removed={2}),
        hg.SetDelta(added={2}, removed={3}),
        None,
    ]

    assert hg.run_graph(union, lhs, rhs) == [6, None, None, None]
    assert hg.run_graph(intersection, lhs, rhs) == [2, 3, 2, 0]
    assert hg.run_graph(difference, lhs, rhs) == [1, 2, 3, None]
    assert hg.run_graph(symmetric_difference, lhs, rhs) == [4, 3, 4, 6]

    @hg.graph
    def recover_union(condition: hg.TS[bool], values: hg.TSS[int], invalid: hg.TSS[int],
                      other: hg.TSS[int]) -> hg.TS[int]:
        selected = hg.operators.if_then_else(condition, values, invalid)
        return hg.operators.sum_(hg.opt(selected) | other)

    assert hg.run_graph(recover_union, [True, False, True], [{1}, None, None], [None, None, None],
                         [{2}, None, None]) == [3, 2, 3]


def test_tss_union_reconciles_reference_retarget_against_the_previous_membership():
    @hg.graph
    def union(condition: hg.TS[bool], lhs: hg.TSS[int], old_target: hg.TSS[int],
              new_target: hg.TSS[int]) -> hg.TSS[int]:
        return lhs | hg.operators.if_then_else(condition, old_target, new_target)

    expected = [
        hg.SetDelta(added={1, 2}),
        hg.SetDelta(added={3}, removed={2}),
    ]
    assert hg.run_graph(union, [True, False], [{1}, None], [{2}, hg.SetDelta(added={3}, removed={2})],
                         [{3}, None]) == expected
    assert hg.run_graph(union, [True, False], [{1}, None], [{2}, None], [{3}, None]) == expected


def test_tss_equality_sees_same_cycle_removals_from_a_sampled_previous_target():
    @hg.graph
    def equal_to_new_target(condition: hg.TS[bool], old_target: hg.TSS[int],
                            new_target: hg.TSS[int]) -> hg.TS[bool]:
        return hg.operators.if_then_else(condition, old_target, new_target) == new_target

    assert hg.run_graph(equal_to_new_target, [True, False],
                         [{2}, hg.SetDelta(added={3}, removed={2})], [{3}, None]) == [False, True]


@pytest.mark.parametrize(("name", "operation", "set_result"), [
    ("union", lambda lhs, rhs: lhs | rhs, True),
    ("intersection", lambda lhs, rhs: lhs & rhs, True),
    ("difference", lambda lhs, rhs: lhs - rhs, True),
    ("symmetric_difference", lambda lhs, rhs: lhs ^ rhs, True),
    ("equal", lambda lhs, rhs: lhs == rhs, False),
    ("not_equal", lambda lhs, rhs: lhs != rhs, False),
])
@pytest.mark.parametrize("optional", [False, True])
def test_sampled_tss_operators_match_seeded_membership_oracle(name, operation, set_result, optional):
    @hg.graph
    def select(which: hg.TS[int], first: hg.TSS[int], second: hg.TSS[int],
               invalid: hg.TSS[int]) -> hg.TSS[int]:
        first_or_second = hg.operators.if_then_else(which == 0, first, second)
        return hg.operators.if_then_else(which == 2, invalid, first_or_second)

    output_type = hg.TSS[int] if set_result else hg.TS[bool]

    @hg.graph
    def evaluate(left_selector: hg.TS[int], right_selector: hg.TS[int], first: hg.TSS[int],
                 second: hg.TSS[int], invalid: hg.TSS[int]) -> output_type:
        lhs = select(left_selector, first, second, invalid)
        rhs = select(right_selector, first, second, invalid)
        return operation(hg.opt(lhs) if optional else lhs, rhs)

    rng = random.Random(0x5A_2026)
    cycle_count = 513
    domains = (tuple(range(130)), tuple(range(1_000, 1_130)))
    source_values = [set(rng.sample(domain, 80)) for domain in domains]
    source_inputs = [[set(values)] for values in source_values]
    source_history = [[set(values) for values in source_values]]
    selectors = [[0, 0] + [rng.randrange(3) for _ in range(cycle_count - 2)] for _ in range(2)]

    for _ in range(1, cycle_count):
        for source_index, domain in enumerate(domains):
            values = source_values[source_index]
            removed = rng.choice(tuple(values))
            added = rng.choice(tuple(set(domain) - values))
            values.remove(removed)
            values.add(added)
            source_inputs[source_index].append(hg.SetDelta(added={added}, removed={removed}))
        source_history.append([set(values) for values in source_values])

    expected = []
    previous_set = None
    previous_bool = None
    for cycle in range(cycle_count):
        left_selection, right_selection = selectors[0][cycle], selectors[1][cycle]
        left = None if left_selection == 2 else source_history[cycle][left_selection]
        right = None if right_selection == 2 else source_history[cycle][right_selection]
        if optional:
            left = left or set()
            right = right or set()

        value = None if left is None or right is None else operation(left, right)
        if set_result:
            if value is None:
                expected.append(None)
                previous_set = None
                continue
            if previous_set is None:
                expected.append(hg.SetDelta(added=value))
            elif value != previous_set:
                expected.append(hg.SetDelta(added=value - previous_set, removed=previous_set - value))
            else:
                expected.append(None)
            previous_set = value
        elif value is None:
            expected.append(None)
            previous_bool = None
        else:
            expected.append((value, previous_bool is None or value != previous_bool))
            previous_bool = value

    actual = hg.run_graph(evaluate, selectors[0], selectors[1], source_inputs[0], source_inputs[1],
                           [None] * cycle_count)
    assert len(actual) == cycle_count, name
    for expected_tick, actual_tick in zip(expected, actual):
        if set_result:
            assert actual_tick == expected_tick, name
        elif expected_tick is None:
            assert actual_tick is None, name
        else:
            expected_value, required_tick = expected_tick
            if required_tick:
                assert actual_tick == expected_value, name
            elif actual_tick is not None:
                assert actual_tick == expected_value, name


def test_collection_reductions_accept_values_and_tss_removals():
    @hg.graph
    def sum_values(values: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.sum_(values=values)

    @hg.graph
    def min_values(values: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.min_(values=values)

    @hg.graph
    def max_values(values: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.max_(values=values)

    @hg.graph
    def mean_values(values: hg.TSS[int]) -> hg.TS[float]:
        return hg.operators.mean(values=values)

    values = [
        {1, 2},
        hg.SetDelta(added={3}, removed={1}),
        hg.SetDelta(removed={2, 3}),
    ]
    assert hg.run_graph(sum_values, values) == [3, 5, 0]
    assert hg.run_graph(min_values, values) == [1, 2, None]
    assert hg.run_graph(max_values, values) == [2, 3, None]
    assert hg.run_graph(mean_values, values) == [1.5, 2.5, None]


def test_optional_collection_sum_publishes_equal_values_for_deduplicated_collection():
    @hg.graph
    def total(values: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.sum_(values=hg.opt(hg.operators.dedup(values)))

    assert hg.run_graph(total, [set(), {0}]) == [0, 0]


def test_collection_reductions_gate_invalid_tsd_items_and_optional_reductions_skip_them():
    @hg.graph
    def sum_values(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.sum_(values=values)

    @hg.graph
    def optional_sum(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.sum_(values=hg.opt(values))

    @hg.graph
    def min_values(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.min_(values=values)

    @hg.graph
    def optional_min(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.min_(values=hg.opt(values))

    @hg.graph
    def max_values(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.max_(values=values)

    @hg.graph
    def optional_max(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.max_(values=hg.opt(values))

    @hg.graph
    def mean_values(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[float]:
        return hg.operators.mean(values=values)

    @hg.graph
    def optional_mean(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[float]:
        return hg.operators.mean(values=hg.opt(values))

    values = [
        {1: 10, 2: 20},
        {1: hg.INVALID, 2: 30},
        {1: hg.REMOVE, 2: hg.INVALID},
    ]
    assert hg.run_graph(sum_values, values) == [30, None, None]
    assert hg.run_graph(optional_sum, values) == [30, 30, 0]
    assert hg.run_graph(min_values, values) == [10, None, None]
    assert hg.run_graph(optional_min, values) == [10, 30, None]
    assert hg.run_graph(max_values, values) == [20, None, None]
    assert hg.run_graph(optional_max, values) == [20, 30, None]
    assert hg.run_graph(mean_values, values) == [15.0, None, None]
    assert hg.run_graph(optional_mean, values) == [15.0, 30.0, None]


def test_numeric_operator_syntax_and_explicit_convert_target():
    @hg.graph
    def divide(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[float]:
        return lhs / rhs

    @hg.graph
    def floor_divide(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return lhs // rhs

    @hg.graph
    def modulo(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return lhs % rhs

    @hg.graph
    def power(value: hg.TS[float]) -> hg.TS[float]:
        return value**2.0

    @hg.graph
    def round_value(value: hg.TS[float]) -> hg.TS[float]:
        return round(value, 2)

    @hg.graph
    def convert(value: hg.TS[int]) -> hg.TS[float]:
        return hg.operators.convert[hg.TS[float]](value)

    assert hg.run_graph(divide, [-7], [3]) == [-7.0 / 3.0]
    assert hg.run_graph(floor_divide, [-7, 7], [3, -3]) == [-3, -3]
    assert hg.run_graph(modulo, [-7, 7], [3, -3]) == [2, -2]
    assert hg.run_graph(power, [-2.0, 3.0]) == [4.0, 9.0]
    assert hg.run_graph(round_value, [2.675, 1.125, 1.375]) == [2.68, 1.12, 1.38]
    assert hg.run_graph(convert, [2, -3]) == [2.0, -3.0]


def test_integer_division_converts_operands_to_binary64_for_full_i64_domain():
    @hg.graph
    def divide(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[float]:
        return lhs / rhs

    minimum = -(1 << 63)
    maximum = (1 << 63) - 1
    two_to_53 = 1 << 53
    two_to_54 = 1 << 54
    lhs = [
        two_to_53 + 1,
        two_to_53 + 3,
        9007199254740993,
        two_to_54 - 1,
        maximum,
        minimum,
        1,
        0,
        27021597764222979,
    ]
    rhs = [two_to_53, two_to_53, 3, 1, 1, -1, maximum, minimum, 864691128455135232]

    random_values = random.Random(0xD1_2026)
    for _ in range(2048):
        lhs.append(random_values.randint(minimum, maximum))
        denominator = random_values.randint(minimum, maximum)
        rhs.append(denominator or 1)

    for _ in range(2048):
        lhs_bits = random_values.randrange(1, 64)
        rhs_bits = random_values.randrange(1, 64)
        numerator = (1 << (lhs_bits - 1)) | random_values.getrandbits(lhs_bits - 1)
        denominator = (1 << (rhs_bits - 1)) | random_values.getrandbits(rhs_bits - 1)
        lhs.append(-numerator if random_values.getrandbits(1) else numerator)
        rhs.append(-denominator if random_values.getrandbits(1) else denominator)

    actual = hg.run_graph(divide, lhs, rhs)
    expected = [float(numerator) / float(denominator) for numerator, denominator in zip(lhs, rhs)]
    assert [value.hex() for value in actual] == [value.hex() for value in expected]

    negative_zero = hg.run_graph(divide, [0], [minimum])[0]
    assert negative_zero == 0.0
    assert math.copysign(1.0, negative_zero) == -1.0


def test_reflected_numeric_operator_syntax():
    @hg.graph
    def divide(value: hg.TS[int]) -> hg.TS[float]:
        return 7 / value

    @hg.graph
    def floor_divide(value: hg.TS[int]) -> hg.TS[int]:
        return 7 // value

    @hg.graph
    def modulo(value: hg.TS[int]) -> hg.TS[int]:
        return 7 % value

    @hg.graph
    def power(value: hg.TS[int]) -> hg.TS[int]:
        return 2**value

    assert hg.run_graph(divide, [2, -2]) == [3.5, -3.5]
    assert hg.run_graph(floor_divide, [2, -2]) == [3, -4]
    assert hg.run_graph(modulo, [3, -3]) == [1, -2]
    assert hg.run_graph(power, [2, 3]) == [4, 8]


def test_mapped_python_operator_wrappers_are_reusable_and_isolated():
    to_float = hg.operators.convert[hg.TS[float]]

    @hg.graph
    def convert_first(value: hg.TS[int]) -> hg.TS[float]:
        return to_float(value)

    @hg.graph
    def convert_second(value: hg.TS[int]) -> hg.TS[float]:
        return to_float(value)

    assert hg.run_graph(convert_first, [1, 2]) == [1.0, 2.0]
    assert hg.run_graph(convert_second, [3, 4]) == [3.0, 4.0]
    with pytest.raises(ValueError, match="does not match operator parameter"):
        to_float[hg.TS[int]]

    @hg.graph
    def explicit_constant() -> hg.TS[bool]:
        return hg.operators.eq[int](1, 1)

    assert hg.run_graph(explicit_constant) == [True]


def test_round_uses_numeric_scaling_ties_to_even_and_handles_extremes():
    @hg.graph
    def round_value(value: hg.TS[float], digits: hg.TS[int]) -> hg.TS[float]:
        return round(value, digits)

    @hg.graph
    def round_whole(value: hg.TS[float]) -> hg.TS[float]:
        return round(value)

    assert hg.run_graph(round_value, [2.675, 1.125, 1.375, -1.125, -1.375], [2, 2, 2, 2, 2]) == [
        2.68,
        1.12,
        1.38,
        -1.12,
        -1.38,
    ]
    assert hg.run_graph(round_whole, [2.5, 3.5, -2.5, -3.5]) == [2.0, 4.0, -2.0, -4.0]
    assert hg.run_graph(round_value, [25.0, 35.0, -25.0, -35.0, 250.0, 350.0], [-1, -1, -1, -1, -2, -2]) == [
        20.0,
        40.0,
        -20.0,
        -40.0,
        200.0,
        400.0,
    ]

    huge_digits = hg.run_graph(round_value, [1.25, -1.25], [sys.maxsize, -(1 << 63)])
    assert huge_digits[0] == 1.25
    assert huge_digits[1] == 0.0 and math.copysign(1.0, huge_digits[1]) == -1.0

    nonfinite = hg.run_graph(round_value, [math.inf, -math.inf, math.nan], [2, -1, 0])
    assert math.isinf(nonfinite[0]) and nonfinite[0] > 0
    assert math.isinf(nonfinite[1]) and nonfinite[1] < 0
    assert math.isnan(nonfinite[2])

    signed_zero = hg.run_graph(round_value, [-0.0, -0.1, 0.1], [2, 0, 0])
    assert math.copysign(1.0, signed_zero[0]) == -1.0
    assert math.copysign(1.0, signed_zero[1]) == -1.0
    assert math.copysign(1.0, signed_zero[2]) == 1.0

    subnormal = math.ulp(0.0)
    assert hg.run_graph(round_value, [1.23e-310, subnormal, subnormal], [310, 323, 324]) == [1e-310, 0.0, subnormal]
    assert hg.run_graph(round_value, [sys.float_info.max], [308]) == [sys.float_info.max]

    with pytest.raises(OverflowError, match="rounded result"):
        hg.run_graph(round_value, [1.5e308], [-308])


def test_convert_requires_a_compatible_explicit_full_time_series_type():
    @hg.graph
    def missing_target(value: hg.TS[int]) -> hg.TS[float]:
        return hg.operators.convert(value)

    @hg.graph
    def wrong_target(value: hg.TS[int]) -> hg.TS[str]:
        return hg.operators.convert[hg.TS[str]](value)

    with pytest.raises(ValueError, match="Cannot resolve operator convert"):
        hg.run_graph(missing_target, [1])

    with pytest.raises(ValueError, match="Cannot resolve operator convert"):
        hg.run_graph(wrong_target, [1])


def test_symbol_equality_handles_scalar_mixed_and_time_series_inputs():
    @hg.graph
    def scalar_equal() -> hg.TS[bool]:
        return hg.operators.eq(hg.Symbol(), hg.symbol(""))

    @hg.graph
    def scalar_not_equal() -> hg.TS[bool]:
        return hg.operators.ne(hg.Symbol(), hg.symbol(""))

    @hg.graph
    def mixed_equal(value: hg.TS[hg.Symbol]) -> hg.TS[bool]:
        return hg.operators.eq(value, hg.symbol(""))

    @hg.graph
    def mixed_not_equal(value: hg.TS[hg.Symbol]) -> hg.TS[bool]:
        return hg.operators.ne(value, hg.symbol(""))

    @hg.graph
    def series_equal(lhs: hg.TS[hg.Symbol], rhs: hg.TS[hg.Symbol]) -> hg.TS[bool]:
        return hg.operators.eq(lhs, rhs)

    @hg.graph
    def series_not_equal(lhs: hg.TS[hg.Symbol], rhs: hg.TS[hg.Symbol]) -> hg.TS[bool]:
        return hg.operators.ne(lhs, rhs)

    values = [hg.Symbol(), hg.symbol(""), hg.symbol("bid")]
    same_or_different = [hg.symbol(""), hg.symbol(""), hg.symbol("ask")]

    assert hg.run_graph(scalar_equal) == [False]
    assert hg.run_graph(scalar_not_equal) == [True]
    assert hg.run_graph(mixed_equal, values) == [False, True, False]
    assert hg.run_graph(mixed_not_equal, values) == [True, False, True]
    assert hg.run_graph(series_equal, values, same_or_different) == [False, True, False]
    assert hg.run_graph(series_not_equal, values, same_or_different) == [True, False, True]


@pytest.mark.parametrize(
    "expression",
    [
        bool,
        lambda point: point and point,
        lambda point: point or point,
        lambda point: not point,
        lambda point: point < 3 < point,
    ],
    ids=["bool", "and", "or", "not", "chained-comparison"],
)
def test_python_wiring_points_reject_truth_conversion(expression):
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return expression(value)

    with pytest.raises(TypeError, match="wiring point has no Python truth value"):
        hg.run_graph(graph, [1])


def test_add_operator_concatenates_python_strings():
    @hg.graph
    def scalar_add() -> hg.TS[str]:
        return hg.operators.add("left", "right")

    @hg.graph
    def series_add(value: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.add(value, "\x00suffix")

    assert hg.run_graph(scalar_add) == ["leftright"]
    assert hg.run_graph(series_add, ["a\x00b", "café"]) == ["a\x00b\x00suffix", "café\x00suffix"]


def test_add_accepts_a_native_const_wiring_point_and_propagates_its_guarantee():
    @hg.graph
    def graph() -> hg.TS[float]:
        constant = hg.operators.const_(4.0)
        result = hg.operators.add(constant, 1.0)
        assert result.type_name == "TS<double, const>"
        return result

    assert hg.run_graph(graph) == [5.0]


def test_wiring_point_addition_uses_the_native_operator():
    @hg.node
    def passthrough(value: float) -> float:
        return value

    @hg.graph
    def add_nested(lhs: hg.TS[float], rhs: float) -> hg.TS[float]:
        return lhs + rhs

    @hg.graph
    def graph(lhs: hg.TS[float], rhs: hg.TS[float]) -> hg.TS[float]:
        left = passthrough(lhs)
        right = passthrough(rhs)
        forward = left + right
        return add_nested(forward, 2.0)

    @hg.graph
    def reflected(value: hg.TS[float]) -> hg.TS[float]:
        return 2.0 + value

    assert hg.run_graph(graph, [1.0, 2.0], [10.0, 20.0]) == [13.0, 24.0]
    assert hg.run_graph(reflected, [3.0, 4.0]) == [5.0, 6.0]


def test_add_operator_accepts_values_dereferenced_from_references():
    @hg.compute_node
    def forward(value: hg.REF[hg.TS[float]]) -> hg.REF[hg.TS[float]]:
        return value.value

    @hg.graph
    def graph(lhs: hg.TS[float], rhs: hg.TS[float]) -> hg.TS[float]:
        return forward(lhs) + forward(rhs)

    assert hg.run_graph(graph, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]


def test_add_observes_retargeted_references_with_equal_values():
    @hg.source_node
    def first() -> hg.TS[float]:
        return 4.0

    @hg.source_node
    def second() -> hg.TS[float]:
        return 4.0

    @hg.compute_node
    def select(
        use_second: hg.TS[bool],
        lhs: hg.REF[hg.TS[float]],
        rhs: hg.REF[hg.TS[float]],
    ) -> hg.REF[hg.TS[float]]:
        return (rhs if use_second.value else lhs).value

    @hg.graph
    def graph(use_second: hg.TS[bool]) -> hg.TS[float]:
        selected = select(use_second, first(), second())
        return hg.operators.add(selected, 1.0)

    assert hg.run_graph(graph, [False, True, False, True]) == [5.0, 5.0, 5.0, 5.0]


def test_reflected_add_preserves_operand_order_and_native_diagnostics():
    @hg.graph
    def unsupported_direct(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.add("unsupported", value)

    @hg.graph
    def unsupported_reflected(value: hg.TS[float]) -> hg.TS[float]:
        return "unsupported" + value

    with pytest.raises(ValueError, match="Cannot resolve operator add") as raised:
        hg.run_graph(unsupported_direct, [1.0])

    with pytest.raises(ValueError, match="Cannot resolve operator add") as reflected:
        hg.run_graph(unsupported_reflected, [1.0])

    assert "Arguments: (lhs: string, rhs:" in str(raised.value)
    assert str(reflected.value) == str(raised.value)


def test_add_operator_preserves_required_input_validity():
    import hgraph._hg_test_extension as extension

    @hg.compute_node
    def to_double(value: hg.TS[int]) -> hg.TS[float]:
        return float(value.value)

    @hg.graph
    def graph(value: hg.TS[int], rhs: hg.TS[float]) -> hg.TS[float]:
        lhs = to_double(extension.invalidate_on_zero(value))
        return lhs + rhs

    assert hg.run_graph(graph, [1, 0, 3], [10.0, 20.0, 30.0]) == [11.0, None, 33.0]


def test_add_does_not_schedule_on_start_for_an_initially_invalid_input():
    @hg.graph
    def graph(value: hg.TS[float]) -> hg.TS[float]:
        return value + 1.0

    assert hg.run_graph(graph, [None, 2.0]) == [None, 3.0]


def test_default_operator_is_available_with_native_fallback_behavior():
    @hg.graph
    def graph(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.default_(value, fallback=5.0)

    assert hg.run_graph(graph, [None, 1.0, 3.0]) == [5.0, 1.0, 3.0]


def test_nothing_operator_is_typed_final_and_never_publishes():
    @hg.graph
    def direct() -> hg.TS[int]:
        return hg.operators.nothing[hg.TS[int]]()

    assert hg.run_graph(direct) == [None]

    required_calls = []

    @hg.compute_node(active=("trigger",))
    def required(trigger: hg.TS[bool], missing: hg.TS[int]) -> hg.TS[bool]:
        required_calls.append(trigger.value)
        return trigger.value

    @hg.graph
    def blocked(trigger: hg.TS[bool]) -> hg.TS[bool]:
        return required(trigger, hg.operators.nothing[hg.TS[int]]())

    assert hg.run_graph(blocked, [True, False]) == [None, None]
    assert required_calls == []

    @hg.compute_node(active=("trigger",), valid=("trigger",))
    def optional(trigger: hg.TS[bool], missing: hg.TS[int]) -> hg.TS[bool]:
        return not missing.valid

    @hg.graph
    def inspect_optional(trigger: hg.TS[bool]) -> hg.TS[bool]:
        return optional(trigger, hg.operators.nothing[hg.TS[int]]())

    assert hg.run_graph(inspect_optional, [True, False]) == [True, True]

    @hg.compute_node(active=("trigger",), valid=("trigger",))
    def collection_size(trigger: hg.TS[bool], missing: hg.TSL[hg.TS[int], 2]) -> hg.TS[bool]:
        return not missing.valid and len(missing) == 2

    @hg.graph
    def inspect_collection(trigger: hg.TS[bool]) -> hg.TS[bool]:
        return collection_size(trigger, hg.operators.nothing[hg.TSL[hg.TS[int], 2]]())

    assert hg.run_graph(inspect_collection, [True, False]) == [True, True]

    @hg.compute_node(active=("trigger",), valid=("trigger",))
    def reference_valid(
        trigger: hg.TS[bool], reference: hg.REF[hg.TS[int]], target: hg.TS[int]
    ) -> hg.TS[bool]:
        return trigger.value and reference.valid and reference.value.valid and not target.valid

    @hg.graph
    def inspect_reference(trigger: hg.TS[bool]) -> hg.TS[bool]:
        missing = hg.operators.nothing[hg.REF[hg.TS[int]]]()
        return reference_valid(trigger, missing, missing)

    assert hg.run_graph(inspect_reference, [True, False]) == [True, False]


def test_add_optional_overload_substitutes_zero_for_invalid_inputs():
    import hgraph._hg_test_extension as extension

    @hg.node
    def to_double(value: int) -> float:
        return float(value)

    @hg.graph
    def optional_series_plus_scalar(value: hg.TS[int]) -> hg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return hg.operators.add(hg.opt(value), 1.0)

    @hg.graph
    def scalar_plus_optional_series(value: hg.TS[int]) -> hg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return 1.0 + hg.opt(value)

    @hg.graph
    def optional_series_plus_series(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[float]:
        lhs = to_double(extension.invalidate_on_zero(lhs))
        rhs = to_double(extension.invalidate_on_zero(rhs))
        return hg.opt(lhs) + rhs

    assert hg.run_graph(optional_series_plus_scalar, [2, 0, 3]) == [3.0, 1.0, 4.0]
    assert hg.run_graph(scalar_plus_optional_series, [2, 0, 3]) == [3.0, 1.0, 4.0]
    assert hg.run_graph(optional_series_plus_series, [2, 0, 3], [10, 20, 0]) == [12.0, 20.0, 3.0]


def test_add_optional_i64_overload_substitutes_zero_for_invalid_inputs():
    import hgraph._hg_test_extension as extension

    @hg.graph
    def graph(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return hg.opt(extension.invalidate_on_zero(lhs)) + hg.opt(extension.invalidate_on_zero(rhs))

    assert hg.run_graph(graph, [1, 0, 3, 0], [10, 20, 0, 0]) == [11, 20, 3, 0]


def test_add_optional_string_overload_substitutes_empty_string_for_invalid_inputs():
    import hgraph._hg_test_extension as extension

    @hg.node
    def to_string(value: int) -> str:
        return str(value)

    @hg.graph
    def graph(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[str]:
        left = to_string(extension.invalidate_on_zero(lhs))
        right = to_string(extension.invalidate_on_zero(rhs))
        return hg.opt(left) + right

    assert hg.run_graph(graph, [1, 0, 3, 0], [10, 20, 0, 0]) == ["110", "20", "3", ""]


def test_optional_string_addition_waits_for_ticks_and_retains_latest_inputs():
    @hg.node
    def to_string(value: int) -> str:
        return str(value)

    @hg.graph
    def graph(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[str]:
        return hg.opt(to_string(lhs)) + hg.opt(to_string(rhs))

    assert hg.run_graph(graph, [None, 1, None, 2], [None, None, 3, None]) == [None, "1", "13", "23"]


def test_wiring_point_addition_selects_optional_overload():
    import hgraph._hg_test_extension as extension

    @hg.node
    def to_double(value: int) -> float:
        return float(value)

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return hg.opt(value) + 1.0

    assert hg.run_graph(graph, [2, 0, 3]) == [3.0, 1.0, 4.0]


def test_wiring_point_addition_does_not_depend_on_python_operator_attributes():
    script = """
import gc
import hgraph as hg
import hgraph._hg as native

del hg.operators.add
del native.operators.add
gc.collect()
hg.operators.add = object()
native.operators.add = object()
gc.collect()

@hg.graph
def graph(lhs: hg.TS[float], rhs: hg.TS[float]) -> hg.TS[float]:
    return lhs + rhs

@hg.graph
def reflected(value: hg.TS[float]) -> hg.TS[float]:
    return 2.0 + value

assert hg.run_graph(graph, [1.0], [2.0]) == [3.0]
assert hg.run_graph(reflected, [3.0]) == [5.0]
"""

    completed = subprocess.run([sys.executable, "-c", script], check=False, capture_output=True, text=True)

    assert completed.returncode == 0, completed.stderr
    assert "nanobind: leaked" not in completed.stderr


def test_add_reports_native_overload_diagnostics_for_unsupported_types():
    @hg.graph
    def graph(lhs: hg.TS[bool], rhs: hg.TS[bool]) -> hg.TS[bool]:
        return hg.operators.add(lhs, rhs)

    with pytest.raises(ValueError, match="Cannot resolve operator add"):
        hg.run_graph(graph, [True], [False])

    @hg.graph
    def mixed(lhs: hg.TS[float], rhs: hg.TS[int]) -> hg.TS[float]:
        return hg.operators.add(lhs, rhs)

    with pytest.raises(ValueError, match="Cannot resolve operator add"):
        hg.run_graph(mixed, [1.0], [2])


def test_operator_namespace_reexports_existing_higher_order_apis():
    import hgraph._hg as native

    assert hg.operators.add is native.operators.add
    assert hg.operators.map is hg.map
    assert hg.operators.mesh is hg.mesh
    assert hg.operators.switch_ is hg.switch_


def test_string_switch_cache_is_released_before_scalar_runtime_shutdown():
    script = """
import hgraph as hg

@hg.graph
def a(value: hg.TS[str]) -> hg.TS[str]:
    return value

@hg.graph
def b(value: hg.TS[str]) -> hg.TS[str]:
    return hg.operators.const_("fallback")

@hg.graph
def root(key: hg.TS[int], value: hg.TS[str]) -> hg.TS[str]:
    return hg.switch_(key, {1: a, hg.DEFAULT: b}, value)

assert hg.run_graph(root, [1, 0], ["original", None]) == ["original", "fallback"]
"""

    completed = subprocess.run([sys.executable, "-c", script], check=False, capture_output=True, text=True)

    assert completed.returncode == 0, completed.stderr


def test_addition_checks_wiring_point_lifetime():
    saved_points = []

    @hg.graph
    def save(value: hg.TS[float]) -> hg.TS[float]:
        saved_points.append(value)
        return value

    assert hg.run_graph(save, [1.0]) == [1.0]

    @hg.graph
    def use_expired(value: hg.TS[float]) -> hg.TS[float]:
        return 1.0 + saved_points[0]

    with pytest.raises(RuntimeError, match="outside its graph construction"):
        hg.run_graph(use_expired, [2.0])
