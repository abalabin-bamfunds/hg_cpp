from datetime import datetime

import pytest

import hg


def test_time_series_set_value_and_delta():
    @hg.compute_node
    def make_set(trigger: hg.TS[int]) -> hg.TSS[int]:
        if trigger.value == 1:
            return hg.SetDelta(added={1, 2})
        return hg.SetDelta(added={3}, removed={1})

    @hg.compute_node
    def inspect(value: hg.TSS[int]) -> hg.TS[object]:
        delta = value.delta_value
        return sum(value.value), frozenset(value.value), hg.SetDelta(delta.added, delta.removed)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_set(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (3, frozenset({1, 2}), hg.SetDelta(added={1, 2})),
        (5, frozenset({2, 3}), hg.SetDelta(added={3}, removed={1})),
    ]


def test_time_series_set_output_accepts_removed_markers():
    @hg.compute_node
    def make_set(trigger: hg.TS[int]) -> hg.TSS[int]:
        if trigger.value == 1:
            return {1, 2}
        if trigger.value == 2:
            return {3, hg.Removed(1)}
        return frozenset({4, hg.Removed(2)})

    @hg.compute_node
    def inspect(value: hg.TSS[int]) -> hg.TS[object]:
        delta = value.delta_value
        return sum(value.value), frozenset(value.value), hg.SetDelta(delta.added, delta.removed)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_set(trigger))

    assert hg.run_graph(graph, [1, 2, 3]) == [
        (3, frozenset({1, 2}), hg.SetDelta(added={1, 2})),
        (5, frozenset({2, 3}), hg.SetDelta(added={3}, removed={1})),
        (7, frozenset({3, 4}), hg.SetDelta(added={4}, removed={2})),
    ]


def test_removed_marker_matches_hgraph_value_semantics():
    removed = hg.Removed(2)

    assert repr(removed) == "Removed(2)"
    assert hash(removed) == hash(2)
    assert removed == 2
    assert 2 == removed
    assert removed == hg.Removed(2)
    assert removed != hg.Removed(3)


def test_input_views_expose_last_modified_time_as_datetime():
    @hg.compute_node
    def inspect(value: hg.TS[int], clock: hg.EvaluationClock = None) -> hg.TS[object]:
        return value.value, value.valid, value.modified, value.last_modified_time, clock.evaluation_time

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[object]:
        return inspect(value)

    actual = hg.run_graph(graph, [1, 2])
    assert [value for value, _, _, _, _ in actual] == [1, 2]
    assert [valid for _, valid, _, _, _ in actual] == [True, True]
    assert [modified for _, _, modified, _, _ in actual] == [True, True]
    assert all(isinstance(timestamp, datetime) for _, _, _, timestamp, _ in actual)
    assert all(timestamp == clock for _, _, _, timestamp, clock in actual)


def test_delta_value_is_none_when_an_input_did_not_change():
    @hg.compute_node
    def make_dictionary(value: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: value.value}

    @hg.compute_node
    def inspect(
        trigger: hg.TS[int],
        value: hg.TS[int],
        dictionary: hg.TSD[int, hg.TS[int]],
    ) -> hg.TS[object]:
        dictionary_delta = dictionary.delta_value
        return (
            trigger.value,
            value.modified,
            value.delta_value,
            dictionary.modified,
            dict(dictionary_delta) if dictionary_delta is not None else None,
        )

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TS[int]) -> hg.TS[object]:
        return inspect(trigger, value, make_dictionary(value))

    assert hg.run_graph(graph, [1, 2], [10, None]) == [
        (1, True, 10, True, {1: 10}),
        (2, False, None, False, None),
    ]


def test_delta_value_is_none_after_input_invalidation():
    observed = []

    @hg.compute_node
    def inspect(trigger: hg.TS[int], value: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        item = value[0]
        observed.append((item.valid, item.modified, item.delta_value))
        return trigger.value

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return inspect(trigger, value)

    assert hg.run_graph(
        graph, [1, 2, 3, 4], [{0: 10, 1: 1}, {0: hg.INVALID}, None, {0: 20}]
    ) == [1, 2, 3, 4]
    assert observed == [(True, True, 10), (False, True, None), (False, False, None), (True, True, 20)]


def test_invalid_set_root_delta_reports_removals_until_the_next_unchanged_cycle():
    @hg.compute_node(valid=())
    def inspect(trigger: hg.TS[int], value: hg.TSS[int]) -> hg.TS[object]:
        delta = value.delta_value
        return (
            trigger.value,
            value.valid,
            value.modified,
            None if delta is None else (sorted(delta.added), sorted(delta.removed)),
            sorted(value.removed()),
        )

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TSS[int]) -> hg.TS[object]:
        return inspect(trigger, hg.opt(value))

    assert hg.run_graph(graph, [1, 2, 3], [hg.SetDelta(added={1, 2}), hg.INVALID, None]) == [
        (1, True, True, ([1, 2], []), []),
        (2, False, True, ([], [1, 2]), [1, 2]),
        (3, False, False, None, []),
    ]


def test_invalid_dictionary_root_delta_reports_removals_until_the_next_unchanged_cycle():
    @hg.compute_node(valid=())
    def inspect(trigger: hg.TS[int], value: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        delta = value.delta_value
        return (
            trigger.value,
            value.valid,
            value.modified,
            None if delta is None else dict(delta),
            list(value.removed_keys()),
        )

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        return inspect(trigger, hg.opt(value))

    assert hg.run_graph(graph, [1, 2, 3], [{1: 10, 2: 20}, hg.INVALID, None]) == [
        (1, True, True, {1: 10, 2: 20}, []),
        (2, False, True, {1: hg.REMOVE, 2: hg.REMOVE}, [1, 2]),
        (3, False, False, None, []),
    ]


def test_dictionary_delta_recurses_into_invalidated_set_items():
    @hg.compute_node(valid=())
    def inspect(trigger: hg.TS[int], value: hg.TSD[int, hg.TSS[int]]) -> hg.TS[object]:
        delta = value.delta_value
        nested_delta = None
        if trigger.value == 2:
            nested = delta[1]
            nested_delta = (sorted(nested.added), sorted(nested.removed))
        return trigger.value, delta is None, nested_delta

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TSD[int, hg.TSS[int]]) -> hg.TS[object]:
        return inspect(trigger, hg.opt(value))

    assert hg.run_graph(graph, [1, 2, 3], [{1: {10, 20}}, {1: hg.INVALID}, None]) == [
        (1, False, None),
        (2, False, ([], [10, 20])),
        (3, True, None),
    ]


def test_dictionary_delta_recurses_into_invalidated_dictionary_items():
    @hg.compute_node(valid=())
    def inspect(trigger: hg.TS[int], value: hg.TSD[int, hg.TSD[int, hg.TS[int]]]) -> hg.TS[object]:
        delta = value.delta_value
        nested_delta = None
        if trigger.value == 2:
            nested_delta = dict(delta[1])
        return trigger.value, delta is None, nested_delta

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TSD[int, hg.TSD[int, hg.TS[int]]]) -> hg.TS[object]:
        return inspect(trigger, hg.opt(value))

    assert hg.run_graph(graph, [1, 2, 3], [{1: {10: 100, 20: 200}}, {1: hg.INVALID}, None]) == [
        (1, False, None),
        (2, False, {10: hg.REMOVE, 20: hg.REMOVE}),
        (3, True, None),
    ]


def test_dictionary_delta_reports_removed_keys_after_reference_invalidation():
    @hg.compute_node
    def select(
        trigger: hg.TS[int], target: hg.REF[hg.TSD[int, hg.TS[int]]]
    ) -> hg.REF[hg.TSD[int, hg.TS[int]]]:
        return target.value

    @hg.compute_node(valid=())
    def inspect(observer: hg.TS[int], value: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        delta = value.delta_value
        return (
            observer.value,
            value.valid,
            value.modified,
            None if delta is None else dict(delta),
        )

    @hg.graph
    def graph(trigger: hg.TS[int], observer: hg.TS[int], value: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        return inspect(observer, hg.opt(select(trigger, value)))

    assert hg.run_graph(
        graph,
        [1, hg.INVALID, None],
        [1, 2, 3],
        [{1: 10, 2: 20}],
    ) == [
        (1, True, True, {1: 10, 2: 20}),
        (2, False, True, {1: hg.REMOVE, 2: hg.REMOVE}),
        (3, False, False, None),
    ]


def test_time_series_set_removed_markers_apply_recursively_in_dictionary():
    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[int, hg.TSS[int]]:
        if trigger.value == 1:
            return {1: {1, 2}}
        return {1: {3, hg.Removed(1)}}

    @hg.compute_node
    def inspect(value: hg.TSD[int, hg.TSS[int]]) -> hg.TS[int]:
        return sum(value[1].value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return inspect(make_dictionary(trigger))

    assert hg.run_graph(graph, [1, 2]) == [3, 5]


def test_fixed_time_series_list_value_delta_and_item_access():
    @hg.compute_node
    def make_list(trigger: hg.TS[int]) -> hg.TSL[hg.TS[int], 3]:
        if trigger.value == 1:
            return {0: 10, 2: 30}
        return {1: 20}

    @hg.compute_node
    def inspect(value: hg.TSL[hg.TS[int], 3]) -> hg.TS[object]:
        return sum(item or 0 for item in value.value), list(value.value), dict(value.delta_value), value[1].valid

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_list(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (40, [10, None, 30], {0: 10, 2: 30}, False),
        (60, [10, 20, 30], {1: 20}, True),
    ]


def test_time_series_bundle_value_delta_inheritance_and_item_access():
    @hg.schema
    class Base:
        left: hg.TS[int]

    @hg.schema
    class Pair(Base):
        right: hg.TS[int]

    @hg.compute_node
    def make_bundle(trigger: hg.TS[int]) -> hg.TSB[Pair]:
        if trigger.value == 1:
            return {"left": 10}
        return {"right": 20}

    @hg.compute_node
    def inspect(value: hg.TSB[Pair]) -> hg.TS[object]:
        return (
            sum(item or 0 for item in value.value.values()),
            dict(value.value),
            dict(value.delta_value),
            value["right"].valid,
        )

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_bundle(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (10, {"left": 10, "right": None}, {"left": 10}, False),
        (30, {"left": 10, "right": 20}, {"right": 20}, True),
    ]


def test_time_series_schemas_keep_nominal_identity_and_support_inheritance():
    def make_pair_schema():
        @hg.schema
        class Pair:
            value: hg.TS[int]

        return Pair

    first_pair = make_pair_schema()
    second_pair = make_pair_schema()

    @hg.source_node
    def make_first_pair() -> hg.TSB[first_pair]:
        return {"value": 10}

    @hg.compute_node
    def read_second_pair(value: hg.TSB[second_pair]) -> hg.TS[int]:
        return value.value.value

    @hg.graph
    def incompatible_graph() -> hg.TS[int]:
        return read_second_pair(make_first_pair())

    with pytest.raises(ValueError):
        hg.run_graph(incompatible_graph)

    @hg.schema
    class Base:
        value: hg.TS[int]

    @hg.schema
    class Derived(Base):
        extra: hg.TS[int]

    @hg.source_node
    def make_derived() -> hg.TSB[Derived]:
        return {"value": 11, "extra": 12}

    @hg.compute_node
    def read_base(value: hg.TSB[Base]) -> hg.TS[int]:
        return value.value.value

    @hg.graph
    def compatible_graph() -> hg.TS[int]:
        return read_base(make_derived())

    assert hg.run_graph(compatible_graph) == [11]


def test_collection_lookup_defaults_and_missing_bundle_attributes():
    @hg.schema
    class Pair:
        left: hg.TS[int]

    @hg.compute_node
    def make_bundle(trigger: hg.TS[int]) -> hg.TSB[Pair]:
        return {"left": trigger.value}

    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: trigger.value}

    @hg.compute_node
    def inspect(bundle: hg.TSB[Pair], dictionary: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        assert bundle.value.get("missing", 7) == 7
        assert dictionary.get(2, 8) == 8
        assert dictionary.value.get(2, 9) == 9
        with pytest.raises(AttributeError):
            _ = bundle.missing
        return bundle.left.value + dictionary[1].value

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return inspect(make_bundle(trigger), make_dictionary(trigger))

    assert hg.run_graph(graph, [1]) == [2]


def test_time_series_schema_rejects_multiple_schema_bases():
    @hg.schema
    class Left:
        left: hg.TS[int]

    @hg.schema
    class Right:
        right: hg.TS[int]

    with pytest.raises(TypeError, match="one schema base"):
        @hg.schema
        class Invalid(Left, Right):
            pass


def test_time_series_dictionary_value_delta_removal_resurrection_and_item_access():
    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 1:
            return {1: 10, 2: 20}
        if trigger.value == 2:
            return {1: hg.REMOVE, 2: 21}
        return {1: 30}

    @hg.compute_node
    def inspect(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        return sum(value.value.values()), dict(value.value), dict(value.delta_value), value[2].value

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_dictionary(trigger))

    assert hg.run_graph(graph, [1, 2, 3]) == [
        (30, {1: 10, 2: 20}, {1: 10, 2: 20}, 20),
        (21, {2: 21}, {1: hg.REMOVE, 2: 21}, 21),
        (51, {1: 30, 2: 21}, {1: 30}, 21),
    ]


def test_string_tsd_keys_named_like_delta_fields_are_not_special():
    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[str, hg.TS[int]]:
        return {"removed": trigger.value, "modified": trigger.value + 1}

    @hg.compute_node
    def inspect(value: hg.TSD[str, hg.TS[int]]) -> hg.TS[int]:
        return value.value["removed"] + value.value["modified"]

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return inspect(make_dictionary(trigger))

    assert hg.run_graph(graph, [1, 3]) == [3, 7]


def test_nested_collection_deltas_are_applied_recursively():
    @hg.compute_node
    def make_nested(trigger: hg.TS[int]) -> hg.TSD[int, hg.TSL[hg.TS[int], 2]]:
        if trigger.value == 1:
            return {1: {0: 10}}
        return {1: {1: 20}}

    @hg.compute_node
    def read(value: hg.TSD[int, hg.TSL[hg.TS[int], 2]]) -> hg.TS[int]:
        return sum(item or 0 for item in value[1].value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return read(make_nested(trigger))

    assert hg.run_graph(graph, [1, 2]) == [10, 30]


def test_nested_collection_delta_views_expose_child_deltas():
    @hg.compute_node
    def make_nested(trigger: hg.TS[int]) -> hg.TSD[int, hg.TSL[hg.TS[int], 2]]:
        if trigger.value == 1:
            return {1: {0: 10}}
        return {1: {1: 20}}

    @hg.compute_node
    def inspect(value: hg.TSD[int, hg.TSL[hg.TS[int], 2]]) -> hg.TS[object]:
        delta = value.delta_value[1]
        if 1 in value.added_keys():
            return sum(item or 0 for item in value[1].value), ("added", list(delta))
        return sum(item or 0 for item in value[1].value), ("updated", dict(delta))

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_nested(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (10, ("added", [10, None])),
        (30, ("updated", {1: 20})),
    ]


def test_reference_delta_can_be_forwarded_and_dereferenced():
    @hg.compute_node
    def forward(value: hg.REF[hg.TS[int]]) -> hg.REF[hg.TS[int]]:
        assert value.value.valid
        return value.value

    @hg.compute_node
    def read(value: hg.TS[int]) -> hg.TS[int]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return read(forward(value))

    assert hg.run_graph(graph, [1, 2, 3]) == [1, 2, 3]


def test_reference_to_a_collection_can_be_forwarded_and_dereferenced():
    @hg.compute_node
    def make_dictionary(value: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: value.value}

    @hg.compute_node
    def forward(
        value: hg.REF[hg.TSD[int, hg.TS[int]]],
    ) -> hg.REF[hg.TSD[int, hg.TS[int]]]:
        return value.value

    @hg.compute_node
    def read(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return value.value[1]

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return read(forward(make_dictionary(value)))

    assert hg.run_graph(graph, [1, 2]) == [1, 2]


def test_tsd_of_references_notifies_a_dereferenced_python_input():
    @hg.compute_node
    def make_references(
        trigger: hg.TS[int],
        left: hg.REF[hg.TS[int]],
        right: hg.REF[hg.TS[int]],
    ) -> hg.TSD[int, hg.REF[hg.TS[int]]]:
        if trigger.value == 1:
            return {1: left.value, 2: right.value}
        return None

    @hg.compute_node
    def read(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return values.value[1] + values.value[2]

    @hg.graph
    def graph(trigger: hg.TS[int], left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return read(make_references(trigger, left, right))

    assert hg.run_graph(graph, [1, 2, 3], [10, 20, 30], [1, 2, 3]) == [11, 22, 33]


def test_delay_retains_target_only_updates_from_a_contained_reference():
    from datetime import timedelta

    @hg.compute_node
    def select_target(
        bind: hg.TS[bool], value: hg.REF[hg.TS[str]]
    ) -> hg.TSD[int, hg.REF[hg.TS[str]]]:
        if bind.value:
            return {1: value.value}

    @hg.compute_node
    def read(values: hg.TSD[int, hg.TS[str]]) -> hg.TS[str]:
        return values.value[1]

    @hg.graph
    def graph(bind: hg.TS[bool], value: hg.TS[str]) -> hg.TS[str]:
        delayed = hg.operators.delay(select_target(bind, value), timedelta(microseconds=2))
        return read(delayed)

    assert hg.run_graph(graph, [True, None, None], ["first", "second", "third"]) == [
        None,
        None,
        "first",
        "second",
        "third",
    ]


def test_sampled_tsd_delta_keeps_removed_keys_available_for_lookup():

    @hg.compute_node
    def first(trigger: hg.TS[bool]) -> hg.TSD[int, hg.TS[int]]:
        return {1: 10, 2: 20, 3: 30}

    @hg.compute_node
    def second(trigger: hg.TS[bool]) -> hg.TSD[int, hg.TS[int]]:
        return {3: 300}

    @hg.compute_node
    def select(
        use_second: hg.TS[bool],
        left: hg.REF[hg.TSD[int, hg.TS[int]]],
        right: hg.REF[hg.TSD[int, hg.TS[int]]],
    ) -> hg.TSD[int, hg.REF[hg.TSD[int, hg.TS[int]]]]:
        return {0: right.value if use_second.value else left.value}

    @hg.compute_node
    def inspect(values: hg.TSD[int, hg.TSD[int, hg.TS[int]]]) -> hg.TS[object]:
        sampled = values[0]
        delta = sampled.delta_value
        return dict(sampled.value), dict(delta), delta[1], delta[2], sum(sampled.value.values())

    @hg.graph
    def graph(use_second: hg.TS[bool]) -> hg.TS[object]:
        return inspect(select(use_second, first(use_second), second(use_second)))

    assert hg.run_graph(graph, [False, True]) == [
        ({1: 10, 2: 20, 3: 30}, {1: 10, 2: 20, 3: 30}, 10, 20, 60),
        ({3: 300}, {1: hg.REMOVE, 2: hg.REMOVE, 3: 300}, hg.REMOVE, hg.REMOVE, 300),
    ]


def test_fixed_list_of_references_can_be_dereferenced_as_one_collection():
    @hg.compute_node
    def make_references(
        trigger: hg.TS[int],
        first: hg.REF[hg.TS[int]],
        second: hg.REF[hg.TS[int]],
    ) -> hg.TSL[hg.REF[hg.TS[int]], 2]:
        if trigger.value == 1:
            return {0: first.value, 1: second.value}
        return None

    @hg.graph
    def graph(trigger: hg.TS[int], first: hg.TS[int], second: hg.TS[int]) -> hg.TS[int]:
        values = make_references(trigger, first, second)
        return add(values[0], values[1])

    @hg.compute_node
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left.value + right.value

    assert hg.run_graph(graph, [1, 2], [10, 20], [1, 2]) == [11, 22]


def test_bundle_reference_field_can_be_dereferenced():
    @hg.schema
    class ReferenceBundle:
        value: hg.REF[hg.TS[int]]

    @hg.compute_node
    def make_bundle(trigger: hg.TS[int], value: hg.REF[hg.TS[int]]) -> hg.TSB[ReferenceBundle]:
        if trigger.value == 1:
            return {"value": value.value}
        return None

    @hg.compute_node
    def read(value: hg.TS[int]) -> hg.TS[int]:
        return value.value

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return read(hg.operators.getattr(make_bundle(trigger, value), hg.symbol("value")))

    assert hg.run_graph(graph, [1, 2], [10, 20]) == [10, 20]


def test_collection_item_and_reference_views_expire_after_the_callback():
    captured_items = []
    captured_references = []

    @hg.compute_node
    def make_list(value: hg.TS[int]) -> hg.TSL[hg.TS[int], 1]:
        return {0: value.value}

    @hg.compute_node
    def inspect(value: hg.TSL[hg.TS[int], 1], reference: hg.REF[hg.TS[int]]) -> hg.TS[int]:
        captured_items.append(value[0])
        captured_references.append(reference.value)
        return value[0].value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return inspect(make_list(value), value)

    assert hg.run_graph(graph, [1]) == [1]
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = captured_items[0].value
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = captured_references[0].valid


def test_lazy_collection_views_and_iterators_expire_after_the_callback():
    captured = {}

    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[int, hg.TSL[hg.TS[int], 1]]:
        return {1: {0: trigger.value}}

    @hg.compute_node
    def inspect(value: hg.TSD[int, hg.TSL[hg.TS[int], 1]]) -> hg.TS[int]:
        captured["input"] = value
        captured["value"] = value.value
        captured["delta"] = value.delta_value
        captured["keys"] = value.keys()
        captured["iterator"] = iter(value.items())
        captured["nested"] = value.value[1]
        next(captured["iterator"])
        return value[1][0].value

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return inspect(make_dictionary(trigger))

    assert hg.run_graph(graph, [1]) == [1]

    expired_accesses = [
        lambda: captured["input"].valid,
        lambda: list(captured["value"]),
        lambda: dict(captured["delta"]),
        lambda: list(captured["keys"]),
        lambda: next(captured["iterator"]),
        lambda: list(captured["nested"]),
    ]
    for access in expired_accesses:
        with pytest.raises(RuntimeError, match="outside its callback"):
            access()


def test_set_delta_views_expire_after_the_callback():
    captured = {}

    @hg.compute_node
    def make_set(trigger: hg.TS[int]) -> hg.TSS[int]:
        return {trigger.value}

    @hg.compute_node
    def inspect(value: hg.TSS[int]) -> hg.TS[int]:
        captured["delta"] = value.delta_value
        captured["added"] = value.delta_value.added
        captured["iterator"] = iter(value.delta_value.added)
        return next(iter(value.value))

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return inspect(make_set(trigger))

    assert hg.run_graph(graph, [1]) == [1]

    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = captured["delta"].added
    with pytest.raises(RuntimeError, match="outside its callback"):
        list(captured["added"])
    with pytest.raises(RuntimeError, match="outside its callback"):
        next(captured["iterator"])


def test_collection_delta_validation_reports_the_shape():
    @hg.source_node
    def invalid() -> hg.TSS[int]:
        return [1, 2]

    @hg.compute_node
    def size(value: hg.TSS[int]) -> hg.TS[int]:
        return len(value.value)

    @hg.graph
    def graph() -> hg.TS[int]:
        return size(invalid())

    with pytest.raises(ValueError, match="TSS output requires"):
        hg.run_graph(graph)


def test_set_delta_fields_do_not_interpret_removed_markers():
    @hg.source_node
    def invalid() -> hg.TSS[int]:
        return hg.SetDelta(added={hg.Removed(1)})

    @hg.compute_node
    def size(value: hg.TSS[int]) -> hg.TS[int]:
        return len(value.value)

    @hg.graph
    def graph() -> hg.TS[int]:
        return size(invalid())

    with pytest.raises(TypeError, match="expected an integer"):
        hg.run_graph(graph)


def test_lazy_tsd_views_cover_removal_key_set_and_expiration():
    captured = []

    @hg.compute_node
    def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 1:
            return {1: 10, 2: 20}
        return {1: hg.REMOVE, 2: 21, 3: 30}

    @hg.compute_node
    def inspect(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        captured.append(values.value)
        delta = values.delta_value
        return (
            sum(values.value.values()),
            dict(values.value),
            values.value.copy(),
            dict(delta),
            list(values.added_keys()),
            list(values.removed_keys()),
            list(values.modified_keys()),
            set(values.key_set.value),
            1 in delta,
            delta.get(1),
        )

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_values(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (30, {1: 10, 2: 20}, {1: 10, 2: 20}, {1: 10, 2: 20}, [1, 2], [], [1, 2], {1, 2}, True, 10),
        (
            51,
            {2: 21, 3: 30},
            {2: 21, 3: 30},
            {1: hg.REMOVE, 2: 21, 3: 30},
            [3],
            [1],
            [2, 3],
            {2, 3},
            True,
            hg.REMOVE,
        ),
    ]
    with pytest.raises(RuntimeError, match="outside its callback"):
        list(captured[0])


def test_tsd_change_views_separate_updates_from_inclusive_modifications():
    @hg.compute_node
    def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 1:
            return {1: 10, 2: 20}
        return {1: hg.REMOVE, 2: 21, 3: 30}

    @hg.compute_node
    def inspect(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        def entry_values(entries):
            return [entry.value if entry.valid else None for entry in entries]

        def entry_items(entries):
            return [(key, value.value if value.valid else None) for key, value in entries]

        return (
            list(values.added_keys()),
            list(values.updated_keys()),
            list(values.modified_keys()),
            list(values.removed_keys()),
            entry_values(values.added_values()),
            entry_values(values.updated_values()),
            entry_values(values.modified_values()),
            entry_values(values.removed_values()),
            entry_items(values.added_items()),
            entry_items(values.updated_items()),
            entry_items(values.modified_items()),
            entry_items(values.removed_items()),
        )

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_values(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (
            [1, 2],
            [],
            [1, 2],
            [],
            [10, 20],
            [],
            [10, 20],
            [],
            [(1, 10), (2, 20)],
            [],
            [(1, 10), (2, 20)],
            [],
        ),
        (
            [3],
            [2],
            [2, 3],
            [1],
            [30],
            [21],
            [21, 30],
            [10],
            [(3, 30)],
            [(2, 21)],
            [(2, 21), (3, 30)],
            [(1, 10)],
        ),
    ]


def test_tsd_updated_selection_membership_and_callback_lifetime():
    captured = []

    @hg.compute_node
    def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: 10, 2: 20} if trigger.value == 1 else {2: 21}

    @hg.compute_node
    def inspect(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        updated_keys = values.updated_keys()
        captured.append(updated_keys)
        captured.append(iter(updated_keys))
        return (list(updated_keys), 2 in updated_keys, 1 in updated_keys, 2 in values.delta_value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_values(trigger))

    assert hg.run_graph(graph, [1, 2]) == [([], False, False, True), ([2], True, False, True)]
    with pytest.raises(RuntimeError, match="outside its callback"):
        list(captured[0])
    with pytest.raises(RuntimeError, match="outside its callback"):
        next(captured[1])


def test_tsd_key_set_tracks_key_changes_and_keeps_its_update_time():
    captured_key_sets = []

    @hg.compute_node
    def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 1:
            return {1: 10}
        if trigger.value == 2:
            return {1: 20}
        if trigger.value == 3:
            return None
        if trigger.value == 4:
            return {1: hg.REMOVE}
        return {1: 30}

    @hg.compute_node
    def inspect(trigger: hg.TS[int], values: hg.TSD[int, hg.TS[int]]) -> hg.TS[object]:
        keys = values.key_set
        delta = keys.delta_value
        captured_key_sets.append(keys.value)
        return (
            trigger.value,
            keys.valid,
            keys.modified,
            set(keys.value),
            None if delta is None else (set(delta.added), set(delta.removed)),
            keys.last_modified_time,
        )

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(trigger, make_values(trigger))

    actual = hg.run_graph(graph, [1, 2, 3, 4, 5])
    assert [state[:5] for state in actual] == [
        (1, True, True, {1}, ({1}, set())),
        (2, True, False, {1}, None),
        (3, True, False, {1}, None),
        (4, True, True, set(), (set(), {1})),
        (5, True, True, {1}, ({1}, set())),
    ]
    assert actual[0][5] == actual[1][5] == actual[2][5]
    assert actual[3][5] > actual[2][5]
    assert actual[4][5] > actual[3][5]
    with pytest.raises(RuntimeError, match="outside its callback"):
        set(captured_key_sets[0])


def test_tss_input_exposes_lazy_changes_and_membership_queries():
    @hg.compute_node
    def make_set(trigger: hg.TS[int]) -> hg.TSS[int]:
        return {1, 2} if trigger.value == 1 else {3, hg.Removed(1)}

    @hg.compute_node
    def inspect(values: hg.TSS[int]) -> hg.TS[object]:
        delta = values.delta_value
        return (
            len(values),
            set(values.value),
            set(delta.added),
            set(delta.removed),
            values.was_added(3),
            values.was_removed(1),
        )

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_set(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (2, {1, 2}, {1, 2}, set(), False, False),
        (2, {2, 3}, {3}, {1}, True, True),
    ]


def test_tsl_input_exposes_child_traversal_and_lazy_values():
    @hg.compute_node
    def make_list(trigger: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        return {0: trigger.value}

    @hg.compute_node
    def inspect(items: hg.TSL[hg.TS[int], 2]) -> hg.TS[object]:
        with pytest.raises(RuntimeError, match="has no value"):
            _ = items[1].value

        return (
            items[0].value,
            [item.valid for item in items],
            list(items.keys()),
            [item.value if item.valid else None for item in items.values()],
            list(items.modified_keys()),
            list(items.valid_keys()),
            list(items.value),
            dict(items.delta_value),
        )

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_list(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (1, [True, False], [0, 1], [1, None], [0], [0], [1, None], {0: 1}),
        (2, [True, False], [0, 1], [2, None], [0], [0], [2, None], {0: 2}),
    ]


def test_tsb_input_exposes_named_child_access_and_lazy_values():
    @hg.schema
    class Pair:
        left: hg.TS[int]
        right: hg.TS[int]

    @hg.compute_node
    def make_bundle(trigger: hg.TS[int]) -> hg.TSB[Pair]:
        return {"left": trigger.value}

    @hg.compute_node
    def inspect(pair: hg.TSB[Pair]) -> hg.TS[object]:
        return (
            pair.left.value,
            pair["left"].value,
            dict(pair.value),
            dict(pair.delta_value),
            list(pair.keys()),
        )

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[object]:
        return inspect(make_bundle(trigger))

    assert hg.run_graph(graph, [1, 2]) == [
        (1, 1, {"left": 1, "right": None}, {"left": 1}, ["left", "right"]),
        (2, 2, {"left": 2, "right": None}, {"left": 2}, ["left", "right"]),
    ]


def test_lazy_values_expose_only_their_collection_protocol():
    @hg.schema
    class Pair:
        left: hg.TS[int]

    @hg.compute_node
    def make_set(trigger: hg.TS[int]) -> hg.TSS[int]:
        return {trigger.value}

    @hg.compute_node
    def make_list(trigger: hg.TS[int]) -> hg.TSL[hg.TS[int], 1]:
        return {0: trigger.value}

    @hg.compute_node
    def make_bundle(trigger: hg.TS[int]) -> hg.TSB[Pair]:
        return {"left": trigger.value}

    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: trigger.value}

    @hg.compute_node
    def inspect(
        set_value: hg.TSS[int],
        list_value: hg.TSL[hg.TS[int], 1],
        bundle_value: hg.TSB[Pair],
        dictionary_value: hg.TSD[int, hg.TS[int]],
    ) -> hg.TS[int]:
        assert not hasattr(set_value.value, "keys")
        assert not hasattr(list_value.value, "keys")
        assert hasattr(bundle_value.value, "keys")
        assert hasattr(dictionary_value.value, "keys")
        assert not hasattr(dictionary_value.value, "left")
        return list_value.value[0]

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return inspect(
            make_set(trigger),
            make_list(trigger),
            make_bundle(trigger),
            make_dictionary(trigger),
        )

    assert hg.run_graph(graph, [1]) == [1]
