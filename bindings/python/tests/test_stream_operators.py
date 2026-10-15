import pytest

import bg


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
    observed = []

    @bg.compute_node
    def make_dictionary(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        if trigger.value == 0:
            return {1: 10, 2: 20}
        if trigger.value == 1:
            return {1: 10, 2: 20}
        if trigger.value == 2:
            return {1: 11}
        return {1: bg.REMOVE, 2: 21, 3: 30}

    @bg.compute_node
    def inspect(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        snapshot = dict(value.value)
        observed.append(snapshot)
        return sum(snapshot.values())

    @bg.graph
    def dedup(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(bg.operators.dedup(make_dictionary(trigger)))

    assert bg.test_graph(dedup, [0, 1, 2, 3]) == [30, None, 31, 51]
    assert observed == [{1: 10, 2: 20}, {1: 11, 2: 20}, {2: 21, 3: 30}]
