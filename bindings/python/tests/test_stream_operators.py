from datetime import timedelta

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

    assert bg.test_graph(take, [10, bg.INVALID, 20, 30, bg.INVALID, 40]) == [10, None, 20, None, None, None]
    assert bg.test_graph(drop, [10, bg.INVALID, 20, 30, bg.INVALID, 40]) == [None, None, None, 30, None, 40]
    assert bg.test_graph(step, [10, 10, 20, bg.INVALID, 30, 40, 50]) == [10, None, 20, None, None, 40, None]


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

    assert bg.test_graph(
        selected,
        [1, 1, 1, 1, 1, 1, 1, 1, 0],
        [10, 20, 30, bg.INVALID, None, 50, None, 60, 70],
    ) == [None, None, 30, None, None, None, 50, None, 70]


def test_valid_and_all_valid_publish_only_validity_changes():
    @bg.graph
    def valid(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.valid(value)

    @bg.graph
    def all_valid(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.all_valid(value)

    assert bg.test_graph(valid, [1, 2, 3]) == [True, None, None]
    assert bg.test_graph(all_valid, [1, 2, 3]) == [True, None, None]


def test_all_valid_checks_every_immediate_item_in_a_fixed_list():
    @bg.compute_node
    def make_list(trigger: bg.TS[int]) -> bg.TSL[bg.TS[int], 2]:
        if trigger.value == 0:
            return {0: 10}
        return {1: 20}

    @bg.graph
    def all_valid(trigger: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.all_valid(make_list(trigger))

    assert bg.test_graph(all_valid, [0, 1]) == [False, True]


def test_sample_uses_passive_value_at_signal_ticks():
    @bg.graph
    def sample(signal: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.sample(signal, value)

    assert bg.test_graph(sample, [0, None, 2, None, 4], [10, 20, None, 30, None]) == [10, None, 20, None, 30]


def test_merge_accepts_optional_inputs():
    @bg.graph
    def merge(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.merge(bg.opt(left), bg.opt(right))

    assert bg.test_graph(merge, [10, None, 30], [100, 200, 300]) == [10, 200, 30]


def test_merge_with_one_input_preserves_invalidation():
    import bg._bg_test_extension as extension

    @bg.graph
    def merge(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.merge(extension.invalidate_on_zero(value)))

    assert bg.test_graph(merge, [10, 0, 30]) == [True, False, True]


def test_merge_rejects_zero_inputs():
    @bg.graph
    def merge() -> bg.TS[int]:
        return bg.operators.merge()

    with pytest.raises(ValueError, match="Cannot resolve operator merge"):
        bg.test_graph(merge)


def test_merge_accepts_a_prebuilt_tsl_by_name():
    @bg.compute_node
    def make_values(left: bg.TS[int], right: bg.TS[int]) -> bg.TSL[bg.TS[int], 2]:
        return {0: left.value, 1: right.value}

    @bg.graph
    def merge_values(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.merge(values=make_values(left, right))

    assert bg.test_graph(merge_values, [1, 2], [10, 20]) == [1, 2]


def test_filter_preserves_output_while_closed_and_skips_unchanged_value_on_reopening():
    @bg.graph
    def filter(condition: bg.TS[bool], value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.filter_(condition, value)

    @bg.graph
    def constant_value(condition: bg.TS[bool]) -> bg.TS[int]:
        return bg.operators.filter_(condition, bg.operators.const_(10))

    assert bg.test_graph(filter, [True, False, False, True, True], [10, 20, 30, 30, 40]) == [
        10,
        None,
        None,
        30,
        40,
    ]
    assert bg.test_graph(constant_value, [True, False, True, True]) == [10, None, None, None]


def test_if_then_else_switches_references_and_follows_only_the_selected_value():
    @bg.graph
    def select(condition: bg.TS[bool], true_value: bg.TS[int], false_value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.if_then_else(condition, true_value, false_value)

    assert bg.test_graph(select, [True, True, False, False, True], [10, 11, 12, 13, 14], [1, 2, 3, 4, 5]) == [
        10,
        11,
        3,
        4,
        14,
    ]

    @bg.graph
    def repeated_selection(condition: bg.TS[bool]) -> bg.TS[int]:
        return bg.operators.if_then_else(condition, bg.operators.const_(10), bg.operators.const_(20))

    assert bg.test_graph(repeated_selection, [True, True, False, False, True]) == [10, None, 20, None, 10]


def test_if_then_else_samples_tss_changes_when_retargeting():
    @bg.graph
    def select(condition: bg.TS[bool], old_target: bg.TSS[int], new_target: bg.TSS[int]) -> bg.TSS[int]:
        return bg.operators.if_then_else(condition, old_target, new_target)

    assert bg.test_graph(select, [True, False], [{2}, bg.SetDelta(added={3}, removed={2})], [{3}, None]) == [
        bg.SetDelta(added={2}),
        bg.SetDelta(added={3}, removed={2}),
    ]


def test_if_then_else_does_not_replay_an_old_tss_delta_when_retargeting():
    @bg.graph
    def select(condition: bg.TS[bool], old_target: bg.TSS[int], new_target: bg.TSS[int]) -> bg.TSS[int]:
        return bg.operators.if_then_else(condition, old_target, new_target)

    assert bg.test_graph(select, [True, True, False],
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
        bg.test_graph(select, [True], [1])


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

    assert bg.test_graph(sample, [0, None, 2, None], [0, 1, 2, 3]) == [30, None, 41, None]


def test_dedup_suppresses_consecutive_equal_values():
    @bg.graph
    def dedup(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.dedup(value)

    assert bg.test_graph(dedup, [1, 1, 2, 2]) == [1, None, 2, None]


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

    assert bg.test_graph(dedup, [0, 1, 2, 3]) == [
        {1: 10, 2: 20},
        None,
        {1: 11, 2: 20},
        {2: 21, 3: 30},
    ]
