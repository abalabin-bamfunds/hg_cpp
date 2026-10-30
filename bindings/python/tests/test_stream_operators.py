from datetime import datetime, timedelta

import pytest

import bg


def test_take_drop_and_step_are_available_to_python_graphs():
    @bg.graph
    def take(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.take(value, 2)

    @bg.graph
    def drop(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.drop(value, 2)

    @bg.graph
    def step(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.step(value, 2)

    assert bg.run_graph(take, [10, bg.INVALID, 20, 30, bg.INVALID, 40]) == [10, None, 20, None, None, None]
    assert bg.run_graph(drop, [10, bg.INVALID, 20, 30, bg.INVALID, 40]) == [None, None, None, 30, None, 40]
    assert bg.run_graph(step, [10, 10, 20, bg.INVALID, 30, 40, 50]) == [10, None, 20, None, None, 40, None]


def test_take_drop_duration_and_slice_are_available_to_python_graphs():
    @bg.graph
    def take(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.take(value, n=timedelta(microseconds=3))

    @bg.graph
    def drop(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.drop(value, n=timedelta(microseconds=3))

    @bg.graph
    def slice(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.slice_(value, 1, 5, 2)

    events = [1, bg.INVALID, 2, bg.INVALID, 3, 4]
    assert bg.run_graph(take, events) == [1, None, 2, None, None, None]
    assert bg.run_graph(drop, events) == [None, None, None, None, 3, 4]
    assert bg.run_graph(slice, [10, bg.INVALID, 20, 30, 40, 50, bg.INVALID, 60]) == [
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
    @bg.graph
    def resample(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.resample(value, timedelta(microseconds=2))

    @bg.graph
    def passthrough(value: bg.TS[int]) -> bg.TS[int]:
        return value

    @bg.graph
    def selected(selector: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(selector, {1: resample, 0: passthrough}, value)

    assert bg.run_graph(
        selected,
        [1, 1, 1, 1, 1, 1, 1, 1, 0],
        [10, 20, 30, bg.INVALID, None, 50, None, 60, 70],
    ) == [None, None, 30, None, None, None, 50, None, 70]


def test_last_modified_time_tracks_input_events_and_invalidation():
    @bg.graph
    def last_modified(value: bg.TS[int]) -> bg.TS[datetime]:
        return bg.operators.last_modified_time(value=value)

    start = datetime(2024, 5, 6, 7, 8, 9, 123456)
    assert bg.run_graph(
        last_modified, [1, 1, None, 2, bg.INVALID, None, 3], start_time=start
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
    @bg.graph
    def elapsed(value: bg.TS[int]) -> bg.TS[timedelta]:
        return bg.operators.elapsed(value)

    start = datetime(2024, 5, 6, 7, 8, 9, 123456)
    assert bg.run_graph(
        elapsed, [1, 1, None, 2, bg.INVALID, 3, 4], start_time=start
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
    @bg.graph
    def debounce(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.debounce(value, timedelta(microseconds=2))

    assert bg.run_graph(debounce, [1, None, None, 2, 3]) == [None, None, 1, None, None, None, 3]


def test_timeout_is_available_to_python_graphs_and_publishes_true_after_silence():
    @bg.graph
    def timeout(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.timeout(value, timedelta(microseconds=2))

    assert bg.run_graph(timeout, [1]) == [None, None, True]


def test_debounce_restarts_on_invalidation_and_revalidation():
    @bg.graph
    def valid_debounce(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.debounce(value, timedelta(microseconds=2)))

    assert bg.run_graph(valid_debounce, [1, None, None, bg.INVALID, None, None, 2]) == [
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
    assert bg.run_graph(valid_debounce, [1, bg.INVALID, 2]) == [False, None, None, None, True]


def test_debounce_reconciles_dictionary_removals_from_a_published_baseline():
    @bg.graph
    def debounce_sum(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return bg.operators.sum_(bg.operators.debounce(value, timedelta(microseconds=2)))

    assert bg.run_graph(debounce_sum, [{1: 10}, None, None, {1: bg.REMOVE}]) == [None, None, 10, None, None, 0]


def test_debounce_timer_is_stopped_and_restarted_with_a_switch_branch():
    @bg.graph
    def debounce(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.debounce(value, timedelta(microseconds=2))

    @bg.graph
    def passthrough(value: bg.TS[int]) -> bg.TS[int]:
        return value

    @bg.graph
    def selected(selector: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(selector, {1: debounce, 0: passthrough}, value)

    assert bg.run_graph(selected, [1, 0, 1], [10, 20, 30]) == [None, 20, None, None, 30]


def test_valid_and_all_valid_publish_only_validity_changes():
    @bg.graph
    def valid(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.valid(value)

    @bg.graph
    def all_valid(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.all_valid(value)

    assert bg.run_graph(valid, [1, 2, 3]) == [True, None, None]
    assert bg.run_graph(all_valid, [1, 2, 3]) == [True, None, None]


def test_all_valid_checks_every_immediate_item_in_a_fixed_list():
    @bg.compute_node
    def make_list(trigger: bg.TS[int]) -> bg.TSL[bg.TS[int], 2]:
        if trigger.value == 0:
            return {0: 10}
        return {1: 20}

    @bg.graph
    def all_valid(trigger: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.all_valid(make_list(trigger))

    assert bg.run_graph(all_valid, [0, 1]) == [False, True]


def test_sample_uses_passive_value_at_signal_ticks():
    @bg.graph
    def sample(signal: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.sample(signal, value)

    assert bg.run_graph(sample, [0, None, 2, None, 4], [10, 20, None, 30, None]) == [10, None, 20, None, 30]


def test_merge_accepts_optional_inputs():
    @bg.graph
    def merge(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.merge(bg.opt(left), bg.opt(right))

    assert bg.run_graph(merge, [10, None, 30], [100, 200, 300]) == [10, 200, 30]


def test_if_routes_its_value_to_one_reference_field():
    @bg.graph
    def true_value(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[int]:
        result = bg.operators.if_(condition, value)
        return bg.operators.getattr(result, bg.symbol("true_value"))

    @bg.graph
    def false_value(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[int]:
        result = bg.operators.if_(condition, value)
        return bg.operators.getattr(result, bg.symbol("false_value"))

    assert bg.run_graph(true_value, [True, False, True], [10, 20, 30]) == [10, None, 30]
    assert bg.run_graph(false_value, [True, False, True], [10, 20, 30]) == [None, 20, None]
    assert bg.run_graph(true_value, [True, bg.INVALID], [10, 20]) == [10, None]

    @bg.graph
    def true_valid(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[bool]:
        result = bg.operators.if_(condition, value)
        return bg.operators.valid(bg.operators.getattr(result, bg.symbol("true_value")))

    assert bg.run_graph(true_valid, [True, False, True], [10, 20, 30]) == [True, False, True]


def test_if_true_emits_distinct_true_ticks_and_supports_one_shot_mode():
    @bg.graph
    def emit_true(condition: bg.TS[bool]) -> bg.TS[bool]:
        return bg.operators.if_true(condition)

    @bg.graph
    def emit_true_once(condition: bg.TS[bool]) -> bg.TS[bool]:
        return bg.operators.if_true(condition, tick_once_only=True)

    assert bg.run_graph(emit_true, [False, True, True, False, True]) == [None, True, True, None, True]
    assert bg.run_graph(emit_true_once, [False, True, True, False, True]) == [None, True, None, None, None]
    assert bg.run_graph(emit_true_once, [True, bg.INVALID, False]) == [True, None, None]


def test_if_true_emits_alternating_true_ticks_from_a_deduplicated_input():
    @bg.graph
    def emit_true(condition: bg.TS[bool]) -> bg.TS[bool]:
        return bg.operators.if_true(bg.operators.dedup(condition))

    assert bg.run_graph(emit_true, [True, True, False, False, True, True]) == [
        True,
        None,
        None,
        None,
        True,
        None,
    ]


def test_race_fails_over_by_continuous_validity_age():
    @bg.graph
    def select(first: bg.TS[int], second: bg.TS[int], third: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.race(first, second, third)

    assert bg.run_graph(
        select,
        [None, None, 10, None],
        [20, None, None, bg.INVALID],
        [None, 30, None, None],
    ) == [20, None, None, 30]


def test_race_uses_leftmost_candidate_for_equal_validity_start_times():
    @bg.graph
    def select(first: bg.TS[int], second: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.race(first, second)

    assert bg.run_graph(select, [10, bg.INVALID, None], [20, None, 30]) == [10, 20, 30]


def test_race_recovers_after_all_candidates_are_invalid():
    @bg.graph
    def select(first: bg.TS[int], second: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.race(first, second)

    assert bg.run_graph(select, [bg.INVALID, 10, bg.INVALID, 30], [bg.INVALID, 20, 25, 40]) == [
        None,
        10,
        25,
        40,
    ]


def test_race_preserves_candidate_age_when_its_valid_reference_retargets():
    @bg.graph
    def select(step: bg.TS[int], route: bg.TS[bool]) -> bg.TS[int]:
        delayed = bg.operators.filter_(step == 1, bg.operators.const_(10))
        retargeted = bg.operators.if_then_else(route, bg.operators.const_(20), bg.operators.const_(30))
        return bg.operators.race(delayed, retargeted)

    assert bg.run_graph(select, [0, 1, 2], [False, False, True]) == [30, None, 20]


def test_race_accepts_a_prebuilt_fixed_list_and_preserves_single_input_identity():
    @bg.graph
    def select_prebuilt(first: bg.TS[int], second: bg.TS[int]) -> bg.TS[int]:
        values = bg.operators.combine[bg.TSL[bg.TS[int], 2]](first, second)
        return bg.operators.race(values=values)

    @bg.graph
    def select_one(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.race(value)

    assert bg.run_graph(select_prebuilt, [10, bg.INVALID], [20, 30]) == [10, 30]
    assert bg.run_graph(select_one, [10, bg.INVALID, 20]) == [10, None, 20]


def test_race_rejects_no_inputs():
    @bg.graph
    def select() -> bg.TS[int]:
        return bg.operators.race()

    with pytest.raises(ValueError, match="Cannot resolve operator race"):
        bg.run_graph(select)


def test_race_selects_a_whole_fixed_list():
    @bg.graph
    def select(first: bg.TSL[bg.TS[int], 2], second: bg.TSL[bg.TS[int], 2]) -> bg.TS[int]:
        values = bg.operators.race(first, second)
        return bg.operators.add(bg.operators.getitem(values, 0), bg.operators.getitem(values, 1))

    assert bg.run_graph(select, [{0: 1, 1: 2}, bg.INVALID], [{0: 10, 1: 20}, {0: 30, 1: 40}]) == [3, 70]


def test_route_by_index_selects_a_fixed_output_slot():
    @bg.graph
    def route(index: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        output = bg.operators.route_by_index[bg.TSL[bg.TS[int], 3]](index, value)
        return bg.operators.getitem(output, 1)

    assert bg.run_graph(route, [1, 8, 1, bg.INVALID, 2], [10, 20, 30, 40, 50]) == [
        10,
        None,
        30,
        None,
        None,
    ]

    @bg.graph
    def slot_valid(index: bg.TS[int], value: bg.TS[int]) -> bg.TS[bool]:
        output = bg.operators.route_by_index[bg.TSL[bg.TS[int], 3]](index, value)
        return bg.operators.valid(bg.operators.getitem(output, 1))

    assert bg.run_graph(slot_valid, [1, -1, 1, bg.INVALID, 2], [10, 20, 30, 40, 50]) == [
        True,
        False,
        True,
        False,
        None,
    ]


def test_route_by_index_requires_an_explicit_fixed_list_result():
    @bg.graph
    def missing_result(index: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.route_by_index(index, value)

    @bg.graph
    def wrong_result(index: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.route_by_index[bg.TS[int]](index, value)

    with pytest.raises(ValueError, match="Cannot resolve operator route_by_index"):
        bg.run_graph(missing_result, [0], [10])
    with pytest.raises(ValueError, match="Cannot resolve operator route_by_index"):
        bg.run_graph(wrong_result, [0], [10])


def test_merge_with_one_input_preserves_invalidation():
    import bg._bg_test_extension as extension

    @bg.graph
    def merge(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.merge(extension.invalidate_on_zero(value)))

    assert bg.run_graph(merge, [10, 0, 30]) == [True, False, True]


def test_merge_rejects_zero_inputs():
    @bg.graph
    def merge() -> bg.TS[int]:
        return bg.operators.merge()

    with pytest.raises(ValueError, match="Cannot resolve operator merge"):
        bg.run_graph(merge)


def test_merge_accepts_a_prebuilt_tsl_by_name():
    @bg.compute_node
    def make_values(left: bg.TS[int], right: bg.TS[int]) -> bg.TSL[bg.TS[int], 2]:
        return {0: left.value, 1: right.value}

    @bg.graph
    def merge_values(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.merge(values=make_values(left, right))

    assert bg.run_graph(merge_values, [1, 2], [10, 20]) == [1, 2]


def test_filter_preserves_output_while_closed_and_skips_unchanged_value_on_reopening():
    @bg.graph
    def filter(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.filter_(condition, value)

    @bg.graph
    def constant_value(condition: bg.TS[bool]) -> bg.TS[int]:
        return bg.operators.filter_(condition, bg.operators.const_(10))

    assert bg.run_graph(filter, [True, False, False, True, True], [10, 20, 30, 30, 40]) == [
        10,
        None,
        None,
        30,
        40,
    ]
    assert bg.run_graph(constant_value, [True, False, True, True]) == [10, None, None, None]


def test_gate_retains_and_replays_each_closed_input_tick():
    @bg.graph
    def gate(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.gate(condition, value)

    assert bg.run_graph(gate, [False, True], [1, 2]) == [None, 1, 2]


def test_batch_flushes_closed_values_when_condition_opens():
    @bg.graph
    def batch(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[tuple[int, ...]]:
        return bg.operators.batch(condition, value, timedelta(microseconds=2))

    assert bg.run_graph(batch, [False, False, True], [1, 2, 3]) == [None, None, (1, 2, 3)]


def test_gate_pauses_and_resumes_after_condition_invalidation():
    @bg.graph
    def gate(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.gate(condition, value)

    assert bg.run_graph(gate, [False, True, bg.INVALID, True], [1, 2, 3, 4]) == [None, 1, None, 2, 3, 4]


def test_batch_waits_for_its_timer_and_retains_values_across_close():
    @bg.graph
    def batch(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[tuple[int, ...]]:
        return bg.operators.batch(condition, value, timedelta(microseconds=2))

    assert bg.run_graph(batch, [True, True, True, True, False, None, True], [1, 2, 3, 4, 5, 6, 7]) == [
        (1,),
        None,
        None,
        (2, 3, 4),
        None,
        None,
        (5, 6, 7),
    ]


def test_batch_owns_strings_and_nested_scalar_values():
    @bg.graph
    def strings(condition: bg.TS[bool], value: bg.TS[str]) -> bg.TS[tuple[str, ...]]:
        return bg.operators.batch(condition, value, timedelta(microseconds=2))

    @bg.graph
    def nested(condition: bg.TS[bool], value: bg.TS[tuple[int, ...]]) -> bg.TS[tuple[tuple[int, ...], ...]]:
        return bg.operators.batch(condition, value, timedelta(microseconds=2))

    assert bg.run_graph(strings, [False, True], ["first", "second"]) == [None, ("first", "second")]
    assert bg.run_graph(nested, [False, True], [(1, 2), (3, 4)]) == [None, ((1, 2), (3, 4))]


def test_if_then_else_switches_references_and_follows_only_the_selected_value():
    @bg.graph
    def select(condition: bg.TS[bool], true_value: bg.TS[int], false_value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.if_then_else(condition, true_value, false_value)

    assert bg.run_graph(select, [True, True, False, False, True], [10, 11, 12, 13, 14], [1, 2, 3, 4, 5]) == [
        10,
        11,
        3,
        4,
        14,
    ]

    @bg.graph
    def repeated_selection(condition: bg.TS[bool]) -> bg.TS[int]:
        return bg.operators.if_then_else(condition, bg.operators.const_(10), bg.operators.const_(20))

    assert bg.run_graph(repeated_selection, [True, True, False, False, True]) == [10, None, 20, None, 10]


def test_if_then_else_samples_tss_changes_when_retargeting():
    @bg.graph
    def select(condition: bg.TS[bool], old_target: bg.TSS[int], new_target: bg.TSS[int]) -> bg.TSS[int]:
        return bg.operators.if_then_else(condition, old_target, new_target)

    assert bg.run_graph(select, [True, False], [{2}, bg.SetDelta(added={3}, removed={2})], [{3}, None]) == [
        bg.SetDelta(added={2}),
        bg.SetDelta(added={3}, removed={2}),
    ]


def test_if_then_else_does_not_replay_an_old_tss_delta_when_retargeting():
    @bg.graph
    def select(condition: bg.TS[bool], old_target: bg.TSS[int], new_target: bg.TSS[int]) -> bg.TSS[int]:
        return bg.operators.if_then_else(condition, old_target, new_target)

    assert bg.run_graph(select, [True, True, False],
                         [{2}, bg.SetDelta(added={3}, removed={2}), None], [{3}, None, None]) == [
        bg.SetDelta(added={2}),
        bg.SetDelta(added={3}, removed={2}),
        bg.SetDelta(),
    ]


def test_if_then_else_rejects_incompatible_branch_structures():
    @bg.compute_node
    def make_set(value: bg.TS[int]) -> bg.TSS[int]:
        return {value.value}

    @bg.graph
    def select(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.if_then_else(condition, value, make_set(value))

    with pytest.raises(ValueError, match="Cannot resolve operator if_then_else"):
        bg.run_graph(select, [True], [1])


def test_sample_copies_the_current_dictionary_value_at_signal_ticks():
    @bg.compute_node
    def make_dictionary(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        if trigger.value == 0:
            return {1: 10, 2: 20}
        if trigger.value == 1:
            return {1: 11, 2: bg.REMOVE}
        if trigger.value == 2:
            return {3: 30}
        return None

    @bg.compute_node
    def total(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return sum(value.value.values())

    @bg.graph
    def sample(signal: bg.TS[int], trigger: bg.TS[int]) -> bg.TS[int]:
        sampled = bg.operators.sample(signal, make_dictionary(trigger))
        return total(sampled)

    assert bg.run_graph(sample, [0, None, 2, None], [0, 1, 2, 3]) == [30, None, 41, None]


def test_dedup_suppresses_consecutive_equal_values():
    @bg.graph
    def dedup(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.dedup(value)

    assert bg.run_graph(dedup, [1, 1, 2, 2]) == [1, None, 2, None]


def test_dedup_suppresses_equal_dictionary_items_and_forwards_membership_changes():
    @bg.compute_node
    def inspect(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[dict[int, int]]:
        return dict(value.value)

    @bg.compute_node
    def make_dictionary(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        if trigger.value == 0:
            return {1: 10, 2: 20}
        if trigger.value == 1:
            return {1: 10, 2: 20}
        if trigger.value == 2:
            return {1: 11}
        return {1: bg.REMOVE, 2: 21, 3: 30}

    @bg.graph
    def dedup(trigger: bg.TS[int]) -> bg.TS[dict[int, int]]:
        return inspect(bg.operators.dedup(make_dictionary(trigger)))

    assert bg.run_graph(dedup, [0, 1, 2, 3]) == [
        {1: 10, 2: 20},
        None,
        {1: 11, 2: 20},
        {2: 21, 3: 30},
    ]
