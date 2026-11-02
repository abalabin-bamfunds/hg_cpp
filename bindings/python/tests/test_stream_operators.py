from datetime import datetime, timedelta

import pytest

import hgraph as hg


def test_take_drop_and_step_are_available_to_python_graphs():
    @hg.graph
    def take(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.take(value, 2)

    @hg.graph
    def drop(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.drop(value, 2)

    @hg.graph
    def step(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.step(value, 2)

    assert hg.run_graph(take, [10, hg.INVALID, 20, 30, hg.INVALID, 40]) == [10, None, 20, None, None, None]
    assert hg.run_graph(drop, [10, hg.INVALID, 20, 30, hg.INVALID, 40]) == [None, None, None, 30, None, 40]
    assert hg.run_graph(step, [10, 10, 20, hg.INVALID, 30, 40, 50]) == [10, None, 20, None, None, 40, None]


def test_take_drop_duration_and_slice_are_available_to_python_graphs():
    @hg.graph
    def take(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.take(value, n=timedelta(microseconds=3))

    @hg.graph
    def drop(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.drop(value, n=timedelta(microseconds=3))

    @hg.graph
    def slice(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.slice_(value, 1, 5, 2)

    events = [1, hg.INVALID, 2, hg.INVALID, 3, 4]
    assert hg.run_graph(take, events) == [1, None, 2, None, None, None]
    assert hg.run_graph(drop, events) == [None, None, None, None, 3, 4]
    assert hg.run_graph(slice, [10, hg.INVALID, 20, 30, 40, 50, hg.INVALID, 60]) == [
        None,
        None,
        20,
        None,
        40,
        None,
        None,
        None,
    ]


def test_resample_is_available_to_python_graphs():
    @hg.graph
    def resample(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.resample(value, timedelta(microseconds=2))

    @hg.graph
    def passthrough(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def selected(selector: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(selector, {1: resample, 0: passthrough}, value)

    assert hg.run_graph(
        selected,
        [1, 1, 1, 1, 1, 1, 1, 1, 0],
        [10, 20, 30, hg.INVALID, None, 50, None, 60, 70],
    ) == [None, None, 30, None, None, None, 50, None, 70]


def test_last_modified_time_tracks_input_events_and_invalidation():
    @hg.graph
    def last_modified(value: hg.TS[int]) -> hg.TS[datetime]:
        return hg.operators.last_modified_time(value=value)

    start = datetime(2024, 5, 6, 7, 8, 9, 123456)
    assert hg.run_graph(
        last_modified, [1, 1, None, 2, hg.INVALID, None, 3], start_time=start
    ) == [
        start,
        start + timedelta(microseconds=1),
        None,
        start + timedelta(microseconds=3),
        None,
        None,
        start + timedelta(microseconds=6),
    ]


def test_elapsed_skips_first_event_and_resets_on_invalidation():
    @hg.graph
    def elapsed(value: hg.TS[int]) -> hg.TS[timedelta]:
        return hg.operators.elapsed(value)

    start = datetime(2024, 5, 6, 7, 8, 9, 123456)
    assert hg.run_graph(
        elapsed, [1, 1, None, 2, hg.INVALID, 3, 4], start_time=start
    ) == [
        None,
        timedelta(microseconds=1),
        None,
        timedelta(microseconds=2),
        None,
        None,
        timedelta(microseconds=1),
    ]


def test_debounce_waits_for_silence_and_is_available_to_python_graphs():
    @hg.graph
    def debounce(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.debounce(value, timedelta(microseconds=2))

    assert hg.run_graph(debounce, [1, None, None, 2, 3]) == [None, None, 1, None, None, None, 3]


def test_timeout_is_available_to_python_graphs_and_publishes_true_after_silence():
    @hg.graph
    def timeout(value: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.timeout(value, timedelta(microseconds=2))

    assert hg.run_graph(timeout, [1]) == [None, None, True]


def test_debounce_restarts_on_invalidation_and_revalidation():
    @hg.graph
    def valid_debounce(value: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.debounce(value, timedelta(microseconds=2)))

    assert hg.run_graph(valid_debounce, [1, None, None, hg.INVALID, None, None, 2]) == [
        False,
        None,
        True,
        None,
        None,
        False,
        None,
        None,
        True,
    ]
    assert hg.run_graph(valid_debounce, [1, hg.INVALID, 2]) == [False, None, None, None, True]


def test_debounce_reconciles_dictionary_removals_from_a_published_baseline():
    @hg.graph
    def debounce_sum(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.sum_(hg.operators.debounce(value, timedelta(microseconds=2)))

    assert hg.run_graph(debounce_sum, [{1: 10}, None, None, {1: hg.REMOVE}]) == [None, None, 10, None, None, 0]


def test_debounce_timer_is_stopped_and_restarted_with_a_switch_branch():
    @hg.graph
    def debounce(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.debounce(value, timedelta(microseconds=2))

    @hg.graph
    def passthrough(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def selected(selector: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(selector, {1: debounce, 0: passthrough}, value)

    assert hg.run_graph(selected, [1, 0, 1], [10, 20, 30]) == [None, 20, None, None, 30]


def test_valid_and_all_valid_publish_only_validity_changes():
    @hg.graph
    def valid(value: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.valid(value)

    @hg.graph
    def all_valid(value: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.all_valid(value)

    assert hg.run_graph(valid, [1, 2, 3]) == [True, None, None]
    assert hg.run_graph(all_valid, [1, 2, 3]) == [True, None, None]


def test_all_valid_checks_every_immediate_item_in_a_fixed_list():
    @hg.compute_node
    def make_list(trigger: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        if trigger.value == 0:
            return {0: 10}
        return {1: 20}

    @hg.graph
    def all_valid(trigger: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.all_valid(make_list(trigger))

    assert hg.run_graph(all_valid, [0, 1]) == [False, True]


def test_sample_uses_passive_value_at_signal_ticks():
    @hg.graph
    def sample(signal: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.sample(signal, value)

    assert hg.run_graph(sample, [0, None, 2, None, 4], [10, 20, None, 30, None]) == [10, None, 20, None, 30]


def test_merge_accepts_optional_inputs():
    @hg.graph
    def merge(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.merge(hg.opt(left), hg.opt(right))

    assert hg.run_graph(merge, [10, None, 30], [100, 200, 300]) == [10, 200, 30]


def test_if_routes_its_value_to_one_reference_field():
    @hg.graph
    def true_value(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[int]:
        result = hg.operators.if_(condition, value)
        return hg.operators.getattr(result, hg.symbol("true_value"))

    @hg.graph
    def false_value(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[int]:
        result = hg.operators.if_(condition, value)
        return hg.operators.getattr(result, hg.symbol("false_value"))

    assert hg.run_graph(true_value, [True, False, True], [10, 20, 30]) == [10, None, 30]
    assert hg.run_graph(false_value, [True, False, True], [10, 20, 30]) == [None, 20, None]
    assert hg.run_graph(true_value, [True, hg.INVALID], [10, 20]) == [10, None]

    @hg.graph
    def true_valid(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[bool]:
        result = hg.operators.if_(condition, value)
        return hg.operators.valid(hg.operators.getattr(result, hg.symbol("true_value")))

    assert hg.run_graph(true_valid, [True, False, True], [10, 20, 30]) == [True, False, True]


def test_if_true_emits_distinct_true_ticks_and_supports_one_shot_mode():
    @hg.graph
    def emit_true(condition: hg.TS[bool]) -> hg.TS[bool]:
        return hg.operators.if_true(condition)

    @hg.graph
    def emit_true_once(condition: hg.TS[bool]) -> hg.TS[bool]:
        return hg.operators.if_true(condition, tick_once_only=True)

    assert hg.run_graph(emit_true, [False, True, True, False, True]) == [None, True, True, None, True]
    assert hg.run_graph(emit_true_once, [False, True, True, False, True]) == [None, True, None, None, None]
    assert hg.run_graph(emit_true_once, [True, hg.INVALID, False]) == [True, None, None]


def test_if_true_emits_alternating_true_ticks_from_a_deduplicated_input():
    @hg.graph
    def emit_true(condition: hg.TS[bool]) -> hg.TS[bool]:
        return hg.operators.if_true(hg.operators.dedup(condition))

    assert hg.run_graph(emit_true, [True, True, False, False, True, True]) == [
        True,
        None,
        None,
        None,
        True,
        None,
    ]


def test_race_fails_over_by_continuous_validity_age():
    @hg.graph
    def select(first: hg.TS[int], second: hg.TS[int], third: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.race(first, second, third)

    assert hg.run_graph(
        select,
        [None, None, 10, None],
        [20, None, None, hg.INVALID],
        [None, 30, None, None],
    ) == [20, None, None, 30]


def test_race_uses_leftmost_candidate_for_equal_validity_start_times():
    @hg.graph
    def select(first: hg.TS[int], second: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.race(first, second)

    assert hg.run_graph(select, [10, hg.INVALID, None], [20, None, 30]) == [10, 20, 30]


def test_race_recovers_after_all_candidates_are_invalid():
    @hg.graph
    def select(first: hg.TS[int], second: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.race(first, second)

    assert hg.run_graph(select, [hg.INVALID, 10, hg.INVALID, 30], [hg.INVALID, 20, 25, 40]) == [
        None,
        10,
        25,
        40,
    ]


def test_race_preserves_candidate_age_when_its_valid_reference_retargets():
    @hg.graph
    def select(step: hg.TS[int], route: hg.TS[bool]) -> hg.TS[int]:
        delayed = hg.operators.filter_(step == 1, hg.operators.const_(10))
        retargeted = hg.operators.if_then_else(route, hg.operators.const_(20), hg.operators.const_(30))
        return hg.operators.race(delayed, retargeted)

    assert hg.run_graph(select, [0, 1, 2], [False, False, True]) == [30, None, 20]


def test_race_accepts_a_prebuilt_fixed_list_and_preserves_single_input_identity():
    @hg.graph
    def select_prebuilt(first: hg.TS[int], second: hg.TS[int]) -> hg.TS[int]:
        values = hg.operators.combine[hg.TSL[hg.TS[int], 2]](first, second)
        return hg.operators.race(values=values)

    @hg.graph
    def select_one(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.race(value)

    assert hg.run_graph(select_prebuilt, [10, hg.INVALID], [20, 30]) == [10, 30]
    assert hg.run_graph(select_one, [10, hg.INVALID, 20]) == [10, None, 20]


def test_race_rejects_no_inputs():
    @hg.graph
    def select() -> hg.TS[int]:
        return hg.operators.race()

    with pytest.raises(ValueError, match="Cannot resolve operator race"):
        hg.run_graph(select)


def test_race_selects_a_whole_fixed_list():
    @hg.graph
    def select(first: hg.TSL[hg.TS[int], 2], second: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        values = hg.operators.race(first, second)
        return hg.operators.add(hg.operators.getitem(values, 0), hg.operators.getitem(values, 1))

    assert hg.run_graph(select, [{0: 1, 1: 2}, hg.INVALID], [{0: 10, 1: 20}, {0: 30, 1: 40}]) == [3, 70]


def test_route_by_index_selects_a_fixed_output_slot():
    @hg.graph
    def route(index: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        output = hg.operators.route_by_index[hg.TSL[hg.TS[int], 3]](index, value)
        return hg.operators.getitem(output, 1)

    assert hg.run_graph(route, [1, 8, 1, hg.INVALID, 2], [10, 20, 30, 40, 50]) == [
        10,
        None,
        30,
        None,
        None,
    ]

    @hg.graph
    def slot_valid(index: hg.TS[int], value: hg.TS[int]) -> hg.TS[bool]:
        output = hg.operators.route_by_index[hg.TSL[hg.TS[int], 3]](index, value)
        return hg.operators.valid(hg.operators.getitem(output, 1))

    assert hg.run_graph(slot_valid, [1, -1, 1, hg.INVALID, 2], [10, 20, 30, 40, 50]) == [
        True,
        False,
        True,
        False,
        None,
    ]


def test_route_by_index_requires_an_explicit_fixed_list_result():
    @hg.graph
    def missing_result(index: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.route_by_index(index, value)

    @hg.graph
    def wrong_result(index: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.route_by_index[hg.TS[int]](index, value)

    with pytest.raises(ValueError, match="Cannot resolve operator route_by_index"):
        hg.run_graph(missing_result, [0], [10])
    with pytest.raises(ValueError, match="Cannot resolve operator route_by_index"):
        hg.run_graph(wrong_result, [0], [10])


def test_merge_with_one_input_preserves_invalidation():
    import hgraph._hg_test_extension as extension

    @hg.graph
    def merge(value: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.merge(extension.invalidate_on_zero(value)))

    assert hg.run_graph(merge, [10, 0, 30]) == [True, False, True]


def test_merge_rejects_zero_inputs():
    @hg.graph
    def merge() -> hg.TS[int]:
        return hg.operators.merge()

    with pytest.raises(ValueError, match="Cannot resolve operator merge"):
        hg.run_graph(merge)


def test_merge_accepts_a_prebuilt_tsl_by_name():
    @hg.compute_node
    def make_values(left: hg.TS[int], right: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        return {0: left.value, 1: right.value}

    @hg.graph
    def merge_values(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.merge(values=make_values(left, right))

    assert hg.run_graph(merge_values, [1, 2], [10, 20]) == [1, 2]


def test_filter_preserves_output_while_closed_and_skips_unchanged_value_on_reopening():
    @hg.graph
    def filter(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.filter_(condition, value)

    @hg.graph
    def constant_value(condition: hg.TS[bool]) -> hg.TS[int]:
        return hg.operators.filter_(condition, hg.operators.const_(10))

    assert hg.run_graph(filter, [True, False, False, True, True], [10, 20, 30, 30, 40]) == [
        10,
        None,
        None,
        30,
        40,
    ]
    assert hg.run_graph(constant_value, [True, False, True, True]) == [10, None, None, None]


def test_gate_retains_and_replays_each_closed_input_tick():
    @hg.graph
    def gate(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.gate(condition, value)

    assert hg.run_graph(gate, [False, True], [1, 2]) == [None, 1, 2]


def test_batch_flushes_closed_values_when_condition_opens():
    @hg.graph
    def batch(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[tuple[int, ...]]:
        return hg.operators.batch(condition, value, timedelta(microseconds=2))

    assert hg.run_graph(batch, [False, False, True], [1, 2, 3]) == [None, None, (1, 2, 3)]


def test_gate_pauses_and_resumes_after_condition_invalidation():
    @hg.graph
    def gate(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.gate(condition, value)

    assert hg.run_graph(gate, [False, True, hg.INVALID, True], [1, 2, 3, 4]) == [None, 1, None, 2, 3, 4]


def test_batch_waits_for_its_timer_and_retains_values_across_close():
    @hg.graph
    def batch(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[tuple[int, ...]]:
        return hg.operators.batch(condition, value, timedelta(microseconds=2))

    assert hg.run_graph(batch, [True, True, True, True, False, None, True], [1, 2, 3, 4, 5, 6, 7]) == [
        (1,),
        None,
        None,
        (2, 3, 4),
        None,
        None,
        (5, 6, 7),
    ]


def test_batch_owns_strings_and_nested_scalar_values():
    @hg.graph
    def strings(condition: hg.TS[bool], value: hg.TS[str]) -> hg.TS[tuple[str, ...]]:
        return hg.operators.batch(condition, value, timedelta(microseconds=2))

    @hg.graph
    def nested(condition: hg.TS[bool], value: hg.TS[tuple[int, ...]]) -> hg.TS[tuple[tuple[int, ...], ...]]:
        return hg.operators.batch(condition, value, timedelta(microseconds=2))

    assert hg.run_graph(strings, [False, True], ["first", "second"]) == [None, ("first", "second")]
    assert hg.run_graph(nested, [False, True], [(1, 2), (3, 4)]) == [None, ((1, 2), (3, 4))]


def test_if_then_else_switches_references_and_follows_only_the_selected_value():
    @hg.graph
    def select(condition: hg.TS[bool], true_value: hg.TS[int], false_value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.if_then_else(condition, true_value, false_value)

    assert hg.run_graph(select, [True, True, False, False, True], [10, 11, 12, 13, 14], [1, 2, 3, 4, 5]) == [
        10,
        11,
        3,
        4,
        14,
    ]

    @hg.graph
    def repeated_selection(condition: hg.TS[bool]) -> hg.TS[int]:
        return hg.operators.if_then_else(condition, hg.operators.const_(10), hg.operators.const_(20))

    assert hg.run_graph(repeated_selection, [True, True, False, False, True]) == [10, None, 20, None, 10]


def test_if_then_else_samples_tss_changes_when_retargeting():
    @hg.graph
    def select(condition: hg.TS[bool], old_target: hg.TSS[int], new_target: hg.TSS[int]) -> hg.TSS[int]:
        return hg.operators.if_then_else(condition, old_target, new_target)

    assert hg.run_graph(select, [True, False], [{2}, hg.SetDelta(added={3}, removed={2})], [{3}, None]) == [
        hg.SetDelta(added={2}),
        hg.SetDelta(added={3}, removed={2}),
    ]


def test_if_then_else_does_not_replay_an_old_tss_delta_when_retargeting():
    @hg.graph
    def select(condition: hg.TS[bool], old_target: hg.TSS[int], new_target: hg.TSS[int]) -> hg.TSS[int]:
        return hg.operators.if_then_else(condition, old_target, new_target)

    assert hg.run_graph(select, [True, True, False],
                         [{2}, hg.SetDelta(added={3}, removed={2}), None], [{3}, None, None]) == [
        hg.SetDelta(added={2}),
        hg.SetDelta(added={3}, removed={2}),
        hg.SetDelta(),
    ]


def test_if_then_else_rejects_incompatible_branch_structures():
    @hg.compute_node
    def make_set(value: hg.TS[int]) -> hg.TSS[int]:
        return {value.value}

    @hg.graph
    def select(condition: hg.TS[bool], value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.if_then_else(condition, value, make_set(value))

    with pytest.raises(ValueError, match="Cannot resolve operator if_then_else"):
        hg.run_graph(select, [True], [1])


def test_sample_copies_the_current_dictionary_value_at_signal_ticks():
    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 0:
            return {1: 10, 2: 20}
        if trigger.value == 1:
            return {1: 11, 2: hg.REMOVE}
        if trigger.value == 2:
            return {3: 30}
        return None

    @hg.compute_node
    def total(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return sum(value.value.values())

    @hg.graph
    def sample(signal: hg.TS[int], trigger: hg.TS[int]) -> hg.TS[int]:
        sampled = hg.operators.sample(signal, make_dictionary(trigger))
        return total(sampled)

    assert hg.run_graph(sample, [0, None, 2, None], [0, 1, 2, 3]) == [30, None, 41, None]


def test_dedup_suppresses_consecutive_equal_values():
    @hg.graph
    def dedup(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.dedup(value)

    assert hg.run_graph(dedup, [1, 1, 2, 2]) == [1, None, 2, None]


def test_dedup_suppresses_equal_dictionary_items_and_forwards_membership_changes():
    @hg.compute_node
    def inspect(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[dict[int, int]]:
        return dict(value.value)

    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 0:
            return {1: 10, 2: 20}
        if trigger.value == 1:
            return {1: 10, 2: 20}
        if trigger.value == 2:
            return {1: 11}
        return {1: hg.REMOVE, 2: 21, 3: 30}

    @hg.graph
    def dedup(trigger: hg.TS[int]) -> hg.TS[dict[int, int]]:
        return inspect(hg.operators.dedup(make_dictionary(trigger)))

    assert hg.run_graph(dedup, [0, 1, 2, 3]) == [
        {1: 10, 2: 20},
        None,
        {1: 11, 2: 20},
        {2: 21, 3: 30},
    ]
