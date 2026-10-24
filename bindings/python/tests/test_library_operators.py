import math
import random
import subprocess
import sys
from typing import Annotated

import pytest

import bg


def test_add_operator_accepts_double_scalars_and_time_series():
    @bg.graph
    def scalar_add() -> bg.TS[float]:
        return bg.operators.add(1.25, 2.5)

    @bg.graph
    def mixed_add(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(value, 2.5)

    @bg.graph
    def reversed_mixed_add(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(2.5, value)

    @bg.graph
    def series_add(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(lhs, rhs)

    @bg.graph
    def keyword_add(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(rhs=rhs, lhs=lhs)

    @bg.graph
    def promoted_add(lhs: float, rhs: float) -> float:
        return bg.operators.add(lhs, rhs)

    assert bg.run_graph(scalar_add) == [3.75]
    assert bg.run_graph(mixed_add, [1.0, 2.0]) == [3.5, 4.5]
    assert bg.run_graph(reversed_mixed_add, [1.0, 2.0]) == [3.5, 4.5]
    assert bg.run_graph(series_add, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]
    assert bg.run_graph(keyword_add, [1.0], [10.0]) == [11.0]
    assert bg.run_graph(promoted_add, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]


def test_add_operator_accepts_i64_scalars_and_time_series():
    @bg.graph
    def scalar_add() -> bg.TS[int]:
        return bg.operators.add(2, 3)

    @bg.graph
    def series_plus_scalar(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.add(value, 3)

    @bg.graph
    def scalar_plus_series(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.add(3, value)

    assert bg.run_graph(scalar_add) == [5]
    assert bg.run_graph(series_plus_scalar, [1, -2]) == [4, 1]
    assert bg.run_graph(scalar_plus_series, [1, -2]) == [4, 1]


def test_i64_arithmetic_wraps_at_signed_boundaries():
    minimum = -(1 << 63)
    maximum = (1 << 63) - 1

    @bg.graph
    def add(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs + rhs

    @bg.graph
    def subtract(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs - rhs

    @bg.graph
    def multiply(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs * rhs

    @bg.graph
    def negate(value: bg.TS[int]) -> bg.TS[int]:
        return -value

    @bg.graph
    def absolute(value: bg.TS[int]) -> bg.TS[int]:
        return abs(value)

    assert bg.run_graph(add, [maximum, minimum], [1, -1]) == [minimum, maximum]
    assert bg.run_graph(subtract, [minimum, maximum], [1, -1]) == [maximum, minimum]
    assert bg.run_graph(multiply, [maximum, minimum], [2, -1]) == [-2, minimum]
    assert bg.run_graph(negate, [minimum, maximum]) == [minimum, -maximum]
    assert bg.run_graph(absolute, [minimum, -maximum]) == [minimum, maximum]


def test_running_numeric_operators_and_named_reset_input():
    @bg.graph
    def sums(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.sums(value)

    @bg.graph
    def mins(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.mins(value)

    @bg.graph
    def maxs(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.maxs(value)

    @bg.graph
    def means(value: bg.TS[int]) -> bg.TS[float]:
        return bg.operators.means(value)

    @bg.graph
    def reset_sums(value: bg.TS[int], reset: bg.TS[bool]) -> bg.TS[int]:
        return bg.operators.sums(value, reset=reset)

    @bg.graph
    def reset_mins(value: bg.TS[int], reset: bg.TS[bool]) -> bg.TS[int]:
        return bg.operators.mins(value, reset=reset)

    @bg.graph
    def reset_maxs(value: bg.TS[int], reset: bg.TS[bool]) -> bg.TS[int]:
        return bg.operators.maxs(value, reset=reset)

    @bg.graph
    def reset_means(value: bg.TS[int], reset: bg.TS[bool]) -> bg.TS[float]:
        return bg.operators.means(value, reset=reset)

    assert bg.run_graph(sums, [1, 1, 2]) == [1, 2, 4]
    assert bg.run_graph(mins, [4.0, 4.0, 6.0, 3.0]) == [4.0, None, None, 3.0]
    assert bg.run_graph(maxs, [4.0, 4.0, 6.0, 3.0]) == [4.0, None, 6.0, None]
    assert bg.run_graph(means, [1, 2, 3]) == [1.0, 1.5, 2.0]
    values = [5, 4, 6, 8, 3, 9, None, 7]
    resets = [None, False, True, None, False, True, True, None]
    assert bg.run_graph(reset_sums, values, resets) == [5, 4, 6, 14, 3, 9, None, 7]
    assert bg.run_graph(reset_mins, values, resets) == [5, 4, 6, None, 3, 9, None, 7]
    assert bg.run_graph(reset_maxs, values, resets) == [5, 4, 6, 8, 3, 9, None, 7]
    assert bg.run_graph(reset_means, values, resets) == [5.0, 4.0, 6.0, 7.0, 3.0, 9.0, None, 7.0]


def test_running_numeric_operators_handle_input_invalidation_with_named_reset():
    @bg.graph
    def sums(value: bg.TS[int], reset: bg.TS[str]) -> bg.TS[int]:
        return bg.operators.sums(value, reset=reset)

    @bg.graph
    def mins(value: bg.TS[int], reset: bg.TS[str]) -> bg.TS[int]:
        return bg.operators.mins(value, reset=reset)

    @bg.graph
    def maxs(value: bg.TS[int], reset: bg.TS[str]) -> bg.TS[int]:
        return bg.operators.maxs(value, reset=reset)

    @bg.graph
    def means(value: bg.TS[int], reset: bg.TS[str]) -> bg.TS[float]:
        return bg.operators.means(value, reset=reset)

    values = [5, 4, bg.INVALID, 3, 2]
    resets = [None, None, None, None, None]
    assert bg.run_graph(sums, values, resets) == [5, 9, None, 3, 5]
    assert bg.run_graph(mins, values, resets) == [5, 4, None, 3, 2]
    assert bg.run_graph(maxs, values, resets) == [5, None, None, 3, None]
    assert bg.run_graph(means, values, resets) == [5.0, 4.5, None, 3.0, 2.5]


def test_running_numeric_operators_preserve_history_on_reset_invalidation():
    @bg.graph
    def sums(value: bg.TS[int], reset: bg.TS[str]) -> bg.TS[int]:
        return bg.operators.sums(value, reset=reset)

    @bg.graph
    def mins(value: bg.TS[int], reset: bg.TS[str]) -> bg.TS[int]:
        return bg.operators.mins(value, reset=reset)

    @bg.graph
    def maxs(value: bg.TS[int], reset: bg.TS[str]) -> bg.TS[int]:
        return bg.operators.maxs(value, reset=reset)

    @bg.graph
    def means(value: bg.TS[int], reset: bg.TS[str]) -> bg.TS[float]:
        return bg.operators.means(value, reset=reset)

    values = [5, 4, None, 3]
    resets = [None, "reset", bg.INVALID, None]
    assert bg.run_graph(sums, values, resets) == [5, 4, None, 7]
    assert bg.run_graph(mins, values, resets) == [5, 4, None, 3]
    assert bg.run_graph(maxs, values, resets) == [5, 4, None, None]
    assert bg.run_graph(means, values, resets) == [5.0, 4.0, None, 3.5]


def test_running_extrema_keep_initial_nan_until_input_invalidation():
    @bg.graph
    def mins(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.mins(value)

    @bg.graph
    def maxs(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.maxs(value)

    @bg.graph
    def mins_valid(value: bg.TS[float]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.mins(value))

    @bg.graph
    def maxs_valid(value: bg.TS[float]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.maxs(value))

    values = [math.nan, 1.0, math.nan, 0.0, bg.INVALID, None, 3.0]
    minimum = bg.run_graph(mins, values)
    maximum = bg.run_graph(maxs, values)
    assert math.isnan(minimum[0])
    assert math.isnan(maximum[0])
    assert minimum[1:] == [None, None, None, None, None, 3.0]
    assert maximum[1:] == [None, None, None, None, None, 3.0]
    assert bg.run_graph(mins_valid, values) == [True, None, None, None, False, None, True]
    assert bg.run_graph(maxs_valid, values) == [True, None, None, None, False, None, True]


def test_running_extrema_ignore_later_nan_without_invalidating_output():
    @bg.graph
    def mins(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.mins(value)

    @bg.graph
    def maxs(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.maxs(value)

    @bg.graph
    def mins_valid(value: bg.TS[float]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.mins(value))

    @bg.graph
    def maxs_valid(value: bg.TS[float]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.maxs(value))

    values = [2.0, math.nan, 3.0, 1.0]
    assert bg.run_graph(mins, values) == [2.0, None, None, 1.0]
    assert bg.run_graph(maxs, values) == [2.0, None, 3.0, None]
    assert bg.run_graph(mins_valid, values) == [True, None, None, None]
    assert bg.run_graph(maxs_valid, values) == [True, None, None, None]


def test_running_extrema_clear_nan_history_on_reset_only_and_simultaneous_ticks():
    @bg.graph
    def mins(value: bg.TS[float], reset: bg.TS[bool]) -> bg.TS[float]:
        return bg.operators.mins(value, reset=reset)

    @bg.graph
    def maxs(value: bg.TS[float], reset: bg.TS[bool]) -> bg.TS[float]:
        return bg.operators.maxs(value, reset=reset)

    @bg.graph
    def mins_valid(value: bg.TS[float], reset: bg.TS[bool]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.mins(value, reset=reset))

    @bg.graph
    def maxs_valid(value: bg.TS[float], reset: bg.TS[bool]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.maxs(value, reset=reset))

    values = [math.nan, 1.0, None, 4.0, math.nan, 5.0, 2.0, 3.0]
    resets = [None, None, True, None, False, None, True, None]
    minimum = bg.run_graph(mins, values, resets)
    maximum = bg.run_graph(maxs, values, resets)
    assert math.isnan(minimum[0])
    assert math.isnan(maximum[0])
    assert minimum[1:4] == [None, None, 4.0]
    assert maximum[1:4] == [None, None, 4.0]
    assert math.isnan(minimum[4])
    assert math.isnan(maximum[4])
    assert minimum[5:] == [None, 2.0, None]
    assert maximum[5:] == [None, 2.0, 3.0]
    expected_validity = [True, None, False, True, None, None, None, None]
    assert bg.run_graph(mins_valid, values, resets) == expected_validity
    assert bg.run_graph(maxs_valid, values, resets) == expected_validity


@pytest.mark.parametrize("operator_name", ["sums", "mins", "maxs", "means"])
@pytest.mark.parametrize("value_type", [bool, str])
def test_running_numeric_operators_reject_non_numeric_time_series(operator_name, value_type):
    operator = getattr(bg.operators, operator_name)

    @bg.graph
    def unsupported(value: bg.TS[value_type]) -> bg.TS[value_type]:
        return operator(value)

    with pytest.raises(ValueError, match=f"Cannot resolve operator {operator_name}"):
        bg.run_graph(unsupported, [True if value_type is bool else "value"])


def test_python_operator_syntax_and_named_boolean_operators():
    @bg.graph
    def arithmetic(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return abs(-(lhs - rhs) * rhs)

    @bg.graph
    def comparison(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.and_(lhs < rhs, bg.operators.not_(rhs < 0))

    assert bg.run_graph(arithmetic, [5.0, 2.0], [2.0, 3.0]) == [6.0, 3.0]
    assert bg.run_graph(comparison, [1, 4, 3], [2, 3, -1]) == [True, False, False]


def test_collection_query_operators_accept_public_keyword_names():
    @bg.schema
    class Pair:
        left: bg.TS[int]
        right: bg.TS[float]

    @bg.graph
    def fixed_list_length(value: bg.TSL[bg.TS[int], 2]) -> bg.TS[int]:
        return bg.operators.len_(value=value)

    @bg.graph
    def fixed_bundle_length(value: bg.TSB[Pair]) -> bg.TS[int]:
        return bg.operators.len_(value=value)

    @bg.graph
    def set_length(value: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.len_(value=value)

    @bg.graph
    def set_contains(collection: bg.TSS[int], key: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=key)

    @bg.graph
    def dictionary_length(value: bg.TS[dict[int, int]]) -> bg.TS[int]:
        return bg.operators.len_(value=value)

    @bg.graph
    def dictionary_contains(collection: bg.TS[dict[int, int]], key: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=key)

    @bg.graph
    def dictionary_contains_scalar(collection: bg.TS[dict[int, int]]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=2)

    assert bg.run_graph(fixed_list_length, [{0: 10, 1: 20}]) == [2]
    assert bg.run_graph(fixed_bundle_length, [{"left": 10, "right": 20.0}]) == [2]
    assert bg.run_graph(set_length, [set(), {1, 2}, bg.SetDelta(added={3}, removed={1})]) == [0, 2, 2]
    assert bg.run_graph(set_contains, [{1}, {2, bg.Removed(1)}, {3, bg.Removed(2)}], [1, 1, 3]) == [True, False, True]
    assert bg.run_graph(dictionary_length, [{1: 10, 2: 20}, {}]) == [2, 0]
    assert bg.run_graph(dictionary_contains, [{1: 10, 2: 20}, {}, {3: 30}], [2, 1, 3]) == [True, False, True]
    assert bg.run_graph(dictionary_contains_scalar, [{1: 10, 2: 20}, {}, {3: 30}]) == [True, False, False]


def test_tsd_contains_operator_distinguishes_physical_and_valid_items_membership():
    @bg.graph
    def physical_contains(collection: bg.TSD[int, bg.TS[int]], key: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=key)

    @bg.graph
    def valid_items_contains(
        collection: Annotated[bg.TSD[int, bg.TS[int]], bg.valid_items], key: bg.TS[int]
    ) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=key)

    dictionary_updates = [{2: 20}, {2: bg.INVALID}, {2: 21}]
    keys = [2, 2, 2]
    assert bg.run_graph(physical_contains, dictionary_updates, keys) == [True, True, True]
    assert bg.run_graph(valid_items_contains, dictionary_updates, keys) == [True, False, True]

    @bg.graph
    def physical_scalar_contains(collection: bg.TSD[int, bg.TS[int]]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=2)

    @bg.graph
    def valid_items_scalar_contains(
        collection: Annotated[bg.TSD[int, bg.TS[int]], bg.valid_items]
    ) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=2)

    unrelated_key_updates = [{2: 20}, {3: 30}, {3: 31}]
    assert bg.run_graph(physical_scalar_contains, unrelated_key_updates) == [True, None, None]
    assert bg.run_graph(valid_items_scalar_contains, unrelated_key_updates) == [True, None, None]


def test_tsd_contains_operator_follows_dynamic_keys_through_absent_key_churn():
    @bg.graph
    def physical_contains(collection: bg.TSD[int, bg.TS[int]], key: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=key)

    @bg.graph
    def valid_items_contains(
        collection: Annotated[bg.TSD[int, bg.TS[int]], bg.valid_items], key: bg.TS[int]
    ) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=key)

    collection = [{2: 20, 3: 30}]
    keys = [2, 4, 5, 3, 4]
    expected = [True, False, False, True, False]
    assert bg.run_graph(physical_contains, collection, keys) == expected
    assert bg.run_graph(valid_items_contains, collection, keys) == expected


def test_tss_equality_and_is_empty():
    @bg.graph
    def equal(lhs: bg.TSS[int], rhs: bg.TSS[int]) -> bg.TS[bool]:
        return lhs == rhs

    @bg.graph
    def empty(values: bg.TSS[int]) -> bg.TS[bool]:
        return bg.operators.is_empty(value=values)

    assert bg.run_graph(equal, [{1}, {2, bg.Removed(1)}, {4}],
                         [{1}, {3, bg.Removed(1)}, {5}]) == [True, False, False]
    assert bg.run_graph(empty, [set(), {1}, bg.SetDelta(removed={1})]) == [True, False, True]


def test_optional_tss_operators_treat_invalid_sets_as_empty():
    @bg.graph
    def equal(condition: bg.TS[bool], values: bg.TSS[int], invalid: bg.TSS[int], empty: bg.TSS[int]) -> bg.TS[bool]:
        selected = bg.operators.if_then_else(condition, values, invalid)
        return bg.operators.eq(lhs=bg.opt(selected), rhs=bg.opt(empty))

    @bg.graph
    def empty(condition: bg.TS[bool], values: bg.TSS[int], invalid: bg.TSS[int]) -> bg.TS[bool]:
        selected = bg.operators.if_then_else(condition, values, invalid)
        return bg.operators.is_empty(value=bg.opt(selected))

    condition = [False, True, True, False]
    values = [None, set(), {1}, None]
    invalid = [None, None, None, None]
    assert bg.run_graph(equal, condition, values, invalid, [set(), None, None, None]) == [True, True, False, True]
    assert bg.run_graph(empty, condition, values, invalid) == [True, True, False, True]


def test_tss_set_operators_and_bitwise_syntax():
    @bg.graph
    def union(lhs: bg.TSS[int], rhs: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.sum_(lhs | rhs)

    @bg.graph
    def intersection(lhs: bg.TSS[int], rhs: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.sum_(lhs & rhs)

    @bg.graph
    def difference(lhs: bg.TSS[int], rhs: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.sum_(lhs - rhs)

    @bg.graph
    def symmetric_difference(lhs: bg.TSS[int], rhs: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.sum_(lhs ^ rhs)

    lhs = [
        {1, 2},
        bg.SetDelta(added={3}, removed={1}),
        None,
        bg.SetDelta(removed={2}),
    ]
    rhs = [
        {2, 3},
        bg.SetDelta(added={1}, removed={2}),
        bg.SetDelta(added={2}, removed={3}),
        None,
    ]

    assert bg.run_graph(union, lhs, rhs) == [6, None, None, None]
    assert bg.run_graph(intersection, lhs, rhs) == [2, 3, 2, 0]
    assert bg.run_graph(difference, lhs, rhs) == [1, 2, 3, None]
    assert bg.run_graph(symmetric_difference, lhs, rhs) == [4, 3, 4, 6]

    @bg.graph
    def recover_union(condition: bg.TS[bool], values: bg.TSS[int], invalid: bg.TSS[int],
                      other: bg.TSS[int]) -> bg.TS[int]:
        selected = bg.operators.if_then_else(condition, values, invalid)
        return bg.operators.sum_(bg.opt(selected) | other)

    assert bg.run_graph(recover_union, [True, False, True], [{1}, None, None], [None, None, None],
                         [{2}, None, None]) == [3, 2, 3]


def test_tss_union_reconciles_reference_retarget_against_the_previous_membership():
    @bg.graph
    def union(condition: bg.TS[bool], lhs: bg.TSS[int], old_target: bg.TSS[int],
              new_target: bg.TSS[int]) -> bg.TSS[int]:
        return lhs | bg.operators.if_then_else(condition, old_target, new_target)

    expected = [
        bg.SetDelta(added={1, 2}),
        bg.SetDelta(added={3}, removed={2}),
    ]
    assert bg.run_graph(union, [True, False], [{1}, None], [{2}, bg.SetDelta(added={3}, removed={2})],
                         [{3}, None]) == expected
    assert bg.run_graph(union, [True, False], [{1}, None], [{2}, None], [{3}, None]) == expected


def test_tss_equality_sees_same_cycle_removals_from_a_sampled_previous_target():
    @bg.graph
    def equal_to_new_target(condition: bg.TS[bool], old_target: bg.TSS[int],
                            new_target: bg.TSS[int]) -> bg.TS[bool]:
        return bg.operators.if_then_else(condition, old_target, new_target) == new_target

    assert bg.run_graph(equal_to_new_target, [True, False],
                         [{2}, bg.SetDelta(added={3}, removed={2})], [{3}, None]) == [False, True]


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
    @bg.graph
    def select(which: bg.TS[int], first: bg.TSS[int], second: bg.TSS[int],
               invalid: bg.TSS[int]) -> bg.TSS[int]:
        first_or_second = bg.operators.if_then_else(which == 0, first, second)
        return bg.operators.if_then_else(which == 2, invalid, first_or_second)

    output_type = bg.TSS[int] if set_result else bg.TS[bool]

    @bg.graph
    def evaluate(left_selector: bg.TS[int], right_selector: bg.TS[int], first: bg.TSS[int],
                 second: bg.TSS[int], invalid: bg.TSS[int]) -> output_type:
        lhs = select(left_selector, first, second, invalid)
        rhs = select(right_selector, first, second, invalid)
        return operation(bg.opt(lhs) if optional else lhs, rhs)

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
            source_inputs[source_index].append(bg.SetDelta(added={added}, removed={removed}))
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
                continue
            if previous_set is None:
                expected.append(bg.SetDelta(added=value))
            elif value != previous_set:
                expected.append(bg.SetDelta(added=value - previous_set, removed=previous_set - value))
            else:
                expected.append(None)
            previous_set = value
        elif value is None:
            expected.append(None)
            previous_bool = None
        else:
            expected.append((value, previous_bool is None or value != previous_bool))
            previous_bool = value

    actual = bg.run_graph(evaluate, selectors[0], selectors[1], source_inputs[0], source_inputs[1],
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
    @bg.graph
    def sum_values(values: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.sum_(values=values)

    @bg.graph
    def min_values(values: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.min_(values=values)

    @bg.graph
    def max_values(values: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.max_(values=values)

    @bg.graph
    def mean_values(values: bg.TSS[int]) -> bg.TS[float]:
        return bg.operators.mean(values=values)

    values = [
        {1, 2},
        bg.SetDelta(added={3}, removed={1}),
        bg.SetDelta(removed={2, 3}),
    ]
    assert bg.run_graph(sum_values, values) == [3, 5, 0]
    assert bg.run_graph(min_values, values) == [1, 2, None]
    assert bg.run_graph(max_values, values) == [2, 3, None]
    assert bg.run_graph(mean_values, values) == [1.5, 2.5, None]


def test_optional_collection_sum_publishes_equal_values_for_deduplicated_collection():
    @bg.graph
    def total(values: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.sum_(values=bg.opt(bg.operators.dedup(values)))

    assert bg.run_graph(total, [set(), {0}]) == [0, 0]


def test_collection_reductions_gate_invalid_tsd_items_and_optional_reductions_skip_them():
    @bg.graph
    def sum_values(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return bg.operators.sum_(values=values)

    @bg.graph
    def optional_sum(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return bg.operators.sum_(values=bg.opt(values))

    @bg.graph
    def min_values(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return bg.operators.min_(values=values)

    @bg.graph
    def optional_min(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return bg.operators.min_(values=bg.opt(values))

    @bg.graph
    def max_values(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return bg.operators.max_(values=values)

    @bg.graph
    def optional_max(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return bg.operators.max_(values=bg.opt(values))

    @bg.graph
    def mean_values(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[float]:
        return bg.operators.mean(values=values)

    @bg.graph
    def optional_mean(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[float]:
        return bg.operators.mean(values=bg.opt(values))

    values = [
        {1: 10, 2: 20},
        {1: bg.INVALID, 2: 30},
        {1: bg.REMOVE, 2: bg.INVALID},
    ]
    assert bg.run_graph(sum_values, values) == [30, None, None]
    assert bg.run_graph(optional_sum, values) == [30, 30, 0]
    assert bg.run_graph(min_values, values) == [10, None, None]
    assert bg.run_graph(optional_min, values) == [10, 30, None]
    assert bg.run_graph(max_values, values) == [20, None, None]
    assert bg.run_graph(optional_max, values) == [20, 30, None]
    assert bg.run_graph(mean_values, values) == [15.0, None, None]
    assert bg.run_graph(optional_mean, values) == [15.0, 30.0, None]


def test_numeric_operator_syntax_and_explicit_convert_target():
    @bg.graph
    def divide(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[float]:
        return lhs / rhs

    @bg.graph
    def floor_divide(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs // rhs

    @bg.graph
    def modulo(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs % rhs

    @bg.graph
    def power(value: bg.TS[float]) -> bg.TS[float]:
        return value**2.0

    @bg.graph
    def round_value(value: bg.TS[float]) -> bg.TS[float]:
        return round(value, 2)

    @bg.graph
    def convert(value: bg.TS[int]) -> bg.TS[float]:
        return bg.operators.convert[bg.TS[float]](value)

    assert bg.run_graph(divide, [-7], [3]) == [-7.0 / 3.0]
    assert bg.run_graph(floor_divide, [-7, 7], [3, -3]) == [-3, -3]
    assert bg.run_graph(modulo, [-7, 7], [3, -3]) == [2, -2]
    assert bg.run_graph(power, [-2.0, 3.0]) == [4.0, 9.0]
    assert bg.run_graph(round_value, [2.675, 1.125, 1.375]) == [2.68, 1.12, 1.38]
    assert bg.run_graph(convert, [2, -3]) == [2.0, -3.0]


def test_integer_division_converts_operands_to_binary64_for_full_i64_domain():
    @bg.graph
    def divide(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[float]:
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

    actual = bg.run_graph(divide, lhs, rhs)
    expected = [float(numerator) / float(denominator) for numerator, denominator in zip(lhs, rhs)]
    assert [value.hex() for value in actual] == [value.hex() for value in expected]

    negative_zero = bg.run_graph(divide, [0], [minimum])[0]
    assert negative_zero == 0.0
    assert math.copysign(1.0, negative_zero) == -1.0


def test_reflected_numeric_operator_syntax():
    @bg.graph
    def divide(value: bg.TS[int]) -> bg.TS[float]:
        return 7 / value

    @bg.graph
    def floor_divide(value: bg.TS[int]) -> bg.TS[int]:
        return 7 // value

    @bg.graph
    def modulo(value: bg.TS[int]) -> bg.TS[int]:
        return 7 % value

    @bg.graph
    def power(value: bg.TS[int]) -> bg.TS[int]:
        return 2**value

    assert bg.run_graph(divide, [2, -2]) == [3.5, -3.5]
    assert bg.run_graph(floor_divide, [2, -2]) == [3, -4]
    assert bg.run_graph(modulo, [3, -3]) == [1, -2]
    assert bg.run_graph(power, [2, 3]) == [4, 8]


def test_mapped_python_operator_wrappers_are_reusable_and_isolated():
    to_float = bg.operators.convert[bg.TS[float]]

    @bg.graph
    def convert_first(value: bg.TS[int]) -> bg.TS[float]:
        return to_float(value)

    @bg.graph
    def convert_second(value: bg.TS[int]) -> bg.TS[float]:
        return to_float(value)

    assert bg.run_graph(convert_first, [1, 2]) == [1.0, 2.0]
    assert bg.run_graph(convert_second, [3, 4]) == [3.0, 4.0]
    with pytest.raises(ValueError, match="does not match operator parameter"):
        to_float[bg.TS[int]]

    @bg.graph
    def explicit_constant() -> bg.TS[bool]:
        return bg.operators.eq[int](1, 1)

    assert bg.run_graph(explicit_constant) == [True]


def test_round_uses_numeric_scaling_ties_to_even_and_handles_extremes():
    @bg.graph
    def round_value(value: bg.TS[float], digits: bg.TS[int]) -> bg.TS[float]:
        return round(value, digits)

    @bg.graph
    def round_whole(value: bg.TS[float]) -> bg.TS[float]:
        return round(value)

    assert bg.run_graph(round_value, [2.675, 1.125, 1.375, -1.125, -1.375], [2, 2, 2, 2, 2]) == [
        2.68,
        1.12,
        1.38,
        -1.12,
        -1.38,
    ]
    assert bg.run_graph(round_whole, [2.5, 3.5, -2.5, -3.5]) == [2.0, 4.0, -2.0, -4.0]
    assert bg.run_graph(round_value, [25.0, 35.0, -25.0, -35.0, 250.0, 350.0], [-1, -1, -1, -1, -2, -2]) == [
        20.0,
        40.0,
        -20.0,
        -40.0,
        200.0,
        400.0,
    ]

    huge_digits = bg.run_graph(round_value, [1.25, -1.25], [sys.maxsize, -(1 << 63)])
    assert huge_digits[0] == 1.25
    assert huge_digits[1] == 0.0 and math.copysign(1.0, huge_digits[1]) == -1.0

    nonfinite = bg.run_graph(round_value, [math.inf, -math.inf, math.nan], [2, -1, 0])
    assert math.isinf(nonfinite[0]) and nonfinite[0] > 0
    assert math.isinf(nonfinite[1]) and nonfinite[1] < 0
    assert math.isnan(nonfinite[2])

    signed_zero = bg.run_graph(round_value, [-0.0, -0.1, 0.1], [2, 0, 0])
    assert math.copysign(1.0, signed_zero[0]) == -1.0
    assert math.copysign(1.0, signed_zero[1]) == -1.0
    assert math.copysign(1.0, signed_zero[2]) == 1.0

    subnormal = math.ulp(0.0)
    assert bg.run_graph(round_value, [1.23e-310, subnormal, subnormal], [310, 323, 324]) == [1e-310, 0.0, subnormal]
    assert bg.run_graph(round_value, [sys.float_info.max], [308]) == [sys.float_info.max]

    with pytest.raises(OverflowError, match="rounded result"):
        bg.run_graph(round_value, [1.5e308], [-308])


def test_convert_requires_a_compatible_explicit_full_time_series_type():
    @bg.graph
    def missing_target(value: bg.TS[int]) -> bg.TS[float]:
        return bg.operators.convert(value)

    @bg.graph
    def wrong_target(value: bg.TS[int]) -> bg.TS[str]:
        return bg.operators.convert[bg.TS[str]](value)

    with pytest.raises(ValueError, match="Cannot resolve operator convert"):
        bg.run_graph(missing_target, [1])

    with pytest.raises(ValueError, match="Cannot resolve operator convert"):
        bg.run_graph(wrong_target, [1])


def test_symbol_equality_handles_scalar_mixed_and_time_series_inputs():
    @bg.graph
    def scalar_equal() -> bg.TS[bool]:
        return bg.operators.eq(bg.Symbol(), bg.symbol(""))

    @bg.graph
    def scalar_not_equal() -> bg.TS[bool]:
        return bg.operators.ne(bg.Symbol(), bg.symbol(""))

    @bg.graph
    def mixed_equal(value: bg.TS[bg.Symbol]) -> bg.TS[bool]:
        return bg.operators.eq(value, bg.symbol(""))

    @bg.graph
    def mixed_not_equal(value: bg.TS[bg.Symbol]) -> bg.TS[bool]:
        return bg.operators.ne(value, bg.symbol(""))

    @bg.graph
    def series_equal(lhs: bg.TS[bg.Symbol], rhs: bg.TS[bg.Symbol]) -> bg.TS[bool]:
        return bg.operators.eq(lhs, rhs)

    @bg.graph
    def series_not_equal(lhs: bg.TS[bg.Symbol], rhs: bg.TS[bg.Symbol]) -> bg.TS[bool]:
        return bg.operators.ne(lhs, rhs)

    values = [bg.Symbol(), bg.symbol(""), bg.symbol("bid")]
    same_or_different = [bg.symbol(""), bg.symbol(""), bg.symbol("ask")]

    assert bg.run_graph(scalar_equal) == [False]
    assert bg.run_graph(scalar_not_equal) == [True]
    assert bg.run_graph(mixed_equal, values) == [False, True, False]
    assert bg.run_graph(mixed_not_equal, values) == [True, False, True]
    assert bg.run_graph(series_equal, values, same_or_different) == [False, True, False]
    assert bg.run_graph(series_not_equal, values, same_or_different) == [True, False, True]


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
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return expression(value)

    with pytest.raises(TypeError, match="wiring point has no Python truth value"):
        bg.run_graph(graph, [1])


def test_add_operator_concatenates_python_strings():
    @bg.graph
    def scalar_add() -> bg.TS[str]:
        return bg.operators.add("left", "right")

    @bg.graph
    def series_add(value: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.add(value, "\x00suffix")

    assert bg.run_graph(scalar_add) == ["leftright"]
    assert bg.run_graph(series_add, ["a\x00b", "café"]) == ["a\x00b\x00suffix", "café\x00suffix"]


def test_add_accepts_a_native_const_wiring_point_and_propagates_its_guarantee():
    @bg.graph
    def graph() -> bg.TS[float]:
        constant = bg.operators.const_(4.0)
        result = bg.operators.add(constant, 1.0)
        assert result.type_name == "TS<double, const>"
        return result

    assert bg.run_graph(graph) == [5.0]


def test_wiring_point_addition_uses_the_native_operator():
    @bg.node
    def passthrough(value: float) -> float:
        return value

    @bg.graph
    def add_nested(lhs: bg.TS[float], rhs: float) -> bg.TS[float]:
        return lhs + rhs

    @bg.graph
    def graph(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        left = passthrough(lhs)
        right = passthrough(rhs)
        forward = left + right
        return add_nested(forward, 2.0)

    @bg.graph
    def reflected(value: bg.TS[float]) -> bg.TS[float]:
        return 2.0 + value

    assert bg.run_graph(graph, [1.0, 2.0], [10.0, 20.0]) == [13.0, 24.0]
    assert bg.run_graph(reflected, [3.0, 4.0]) == [5.0, 6.0]


def test_add_operator_accepts_values_dereferenced_from_references():
    @bg.compute_node
    def forward(value: bg.REF[bg.TS[float]]) -> bg.REF[bg.TS[float]]:
        return value.value

    @bg.graph
    def graph(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return forward(lhs) + forward(rhs)

    assert bg.run_graph(graph, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]


def test_add_observes_retargeted_references_with_equal_values():
    @bg.source_node
    def first() -> bg.TS[float]:
        return 4.0

    @bg.source_node
    def second() -> bg.TS[float]:
        return 4.0

    @bg.compute_node
    def select(
        use_second: bg.TS[bool],
        lhs: bg.REF[bg.TS[float]],
        rhs: bg.REF[bg.TS[float]],
    ) -> bg.REF[bg.TS[float]]:
        return (rhs if use_second.value else lhs).value

    @bg.graph
    def graph(use_second: bg.TS[bool]) -> bg.TS[float]:
        selected = select(use_second, first(), second())
        return bg.operators.add(selected, 1.0)

    assert bg.run_graph(graph, [False, True, False, True]) == [5.0, 5.0, 5.0, 5.0]


def test_reflected_add_preserves_operand_order_and_native_diagnostics():
    @bg.graph
    def unsupported_direct(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add("unsupported", value)

    @bg.graph
    def unsupported_reflected(value: bg.TS[float]) -> bg.TS[float]:
        return "unsupported" + value

    with pytest.raises(ValueError, match="Cannot resolve operator add") as raised:
        bg.run_graph(unsupported_direct, [1.0])

    with pytest.raises(ValueError, match="Cannot resolve operator add") as reflected:
        bg.run_graph(unsupported_reflected, [1.0])

    assert "Arguments: (lhs: string, rhs:" in str(raised.value)
    assert str(reflected.value) == str(raised.value)


def test_add_operator_preserves_required_input_validity():
    import bg._bg_test_extension as extension

    @bg.compute_node
    def to_double(value: bg.TS[int]) -> bg.TS[float]:
        return float(value.value)

    @bg.graph
    def graph(value: bg.TS[int], rhs: bg.TS[float]) -> bg.TS[float]:
        lhs = to_double(extension.invalidate_on_zero(value))
        return lhs + rhs

    assert bg.run_graph(graph, [1, 0, 3], [10.0, 20.0, 30.0]) == [11.0, None, 33.0]


def test_add_does_not_schedule_on_start_for_an_initially_invalid_input():
    @bg.graph
    def graph(value: bg.TS[float]) -> bg.TS[float]:
        return value + 1.0

    assert bg.run_graph(graph, [None, 2.0]) == [None, 3.0]


def test_default_operator_is_available_with_native_fallback_behavior():
    @bg.graph
    def graph(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.default_(value, fallback=5.0)

    assert bg.run_graph(graph, [None, 1.0, 3.0]) == [5.0, 1.0, 3.0]


def test_add_optional_overload_substitutes_zero_for_invalid_inputs():
    import bg._bg_test_extension as extension

    @bg.node
    def to_double(value: int) -> float:
        return float(value)

    @bg.graph
    def optional_series_plus_scalar(value: bg.TS[int]) -> bg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return bg.operators.add(bg.opt(value), 1.0)

    @bg.graph
    def scalar_plus_optional_series(value: bg.TS[int]) -> bg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return 1.0 + bg.opt(value)

    @bg.graph
    def optional_series_plus_series(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[float]:
        lhs = to_double(extension.invalidate_on_zero(lhs))
        rhs = to_double(extension.invalidate_on_zero(rhs))
        return bg.opt(lhs) + rhs

    assert bg.run_graph(optional_series_plus_scalar, [2, 0, 3]) == [3.0, 1.0, 4.0]
    assert bg.run_graph(scalar_plus_optional_series, [2, 0, 3]) == [3.0, 1.0, 4.0]
    assert bg.run_graph(optional_series_plus_series, [2, 0, 3], [10, 20, 0]) == [12.0, 20.0, 3.0]


def test_add_optional_i64_overload_substitutes_zero_for_invalid_inputs():
    import bg._bg_test_extension as extension

    @bg.graph
    def graph(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return bg.opt(extension.invalidate_on_zero(lhs)) + bg.opt(extension.invalidate_on_zero(rhs))

    assert bg.run_graph(graph, [1, 0, 3, 0], [10, 20, 0, 0]) == [11, 20, 3, 0]


def test_add_optional_string_overload_substitutes_empty_string_for_invalid_inputs():
    import bg._bg_test_extension as extension

    @bg.node
    def to_string(value: int) -> str:
        return str(value)

    @bg.graph
    def graph(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[str]:
        left = to_string(extension.invalidate_on_zero(lhs))
        right = to_string(extension.invalidate_on_zero(rhs))
        return bg.opt(left) + right

    assert bg.run_graph(graph, [1, 0, 3, 0], [10, 20, 0, 0]) == ["110", "20", "3", ""]


def test_optional_string_addition_waits_for_ticks_and_retains_latest_inputs():
    @bg.node
    def to_string(value: int) -> str:
        return str(value)

    @bg.graph
    def graph(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[str]:
        return bg.opt(to_string(lhs)) + bg.opt(to_string(rhs))

    assert bg.run_graph(graph, [None, 1, None, 2], [None, None, 3, None]) == [None, "1", "13", "23"]


def test_wiring_point_addition_selects_optional_overload():
    import bg._bg_test_extension as extension

    @bg.node
    def to_double(value: int) -> float:
        return float(value)

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return bg.opt(value) + 1.0

    assert bg.run_graph(graph, [2, 0, 3]) == [3.0, 1.0, 4.0]


def test_wiring_point_addition_does_not_depend_on_python_operator_attributes():
    script = """
import gc
import bg
import bg._bg as native

del bg.operators.add
del native.operators.add
gc.collect()
bg.operators.add = object()
native.operators.add = object()
gc.collect()

@bg.graph
def graph(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
    return lhs + rhs

@bg.graph
def reflected(value: bg.TS[float]) -> bg.TS[float]:
    return 2.0 + value

assert bg.run_graph(graph, [1.0], [2.0]) == [3.0]
assert bg.run_graph(reflected, [3.0]) == [5.0]
"""

    completed = subprocess.run([sys.executable, "-c", script], check=False, capture_output=True, text=True)

    assert completed.returncode == 0, completed.stderr
    assert "nanobind: leaked" not in completed.stderr


def test_add_reports_native_overload_diagnostics_for_unsupported_types():
    @bg.graph
    def graph(lhs: bg.TS[bool], rhs: bg.TS[bool]) -> bg.TS[bool]:
        return bg.operators.add(lhs, rhs)

    with pytest.raises(ValueError, match="Cannot resolve operator add"):
        bg.run_graph(graph, [True], [False])

    @bg.graph
    def mixed(lhs: bg.TS[float], rhs: bg.TS[int]) -> bg.TS[float]:
        return bg.operators.add(lhs, rhs)

    with pytest.raises(ValueError, match="Cannot resolve operator add"):
        bg.run_graph(mixed, [1.0], [2])


def test_operator_namespace_reexports_existing_higher_order_apis():
    import bg._bg as native

    assert bg.operators.add is native.operators.add
    assert bg.operators.map is bg.map
    assert bg.operators.mesh is bg.mesh
    assert bg.operators.switch_ is bg.switch_


def test_string_switch_cache_is_released_before_scalar_runtime_shutdown():
    script = """
import bg

@bg.graph
def a(value: bg.TS[str]) -> bg.TS[str]:
    return value

@bg.graph
def b(value: bg.TS[str]) -> bg.TS[str]:
    return bg.operators.const_("fallback")

@bg.graph
def root(key: bg.TS[int], value: bg.TS[str]) -> bg.TS[str]:
    return bg.switch_(key, {1: a, bg.DEFAULT: b}, value)

assert bg.run_graph(root, [1, 0], ["original", None]) == ["original", "fallback"]
"""

    completed = subprocess.run([sys.executable, "-c", script], check=False, capture_output=True, text=True)

    assert completed.returncode == 0, completed.stderr


def test_addition_checks_wiring_point_lifetime():
    saved_points = []

    @bg.graph
    def save(value: bg.TS[float]) -> bg.TS[float]:
        saved_points.append(value)
        return value

    assert bg.run_graph(save, [1.0]) == [1.0]

    @bg.graph
    def use_expired(value: bg.TS[float]) -> bg.TS[float]:
        return 1.0 + saved_points[0]

    with pytest.raises(RuntimeError, match="outside its graph construction"):
        bg.run_graph(use_expired, [2.0])
