import pytest

import bg


def test_time_series_set_value_and_delta():
    observed = []

    @bg.compute_node
    def make_set(trigger: bg.TS[int]) -> bg.TSS[int]:
        if trigger.value == 1:
            return bg.SetDelta(added={1, 2})
        return bg.SetDelta(added={3}, removed={1})

    @bg.compute_node
    def inspect(value: bg.TSS[int]) -> bg.TS[int]:
        observed.append((value.value, value.delta_value))
        return sum(value.value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_set(trigger))

    assert bg.test_graph(graph, [1, 2]) == [3, 5]
    assert observed == [
        (frozenset({1, 2}), bg.SetDelta(added={1, 2})),
        (frozenset({2, 3}), bg.SetDelta(added={3}, removed={1})),
    ]


def test_time_series_set_output_accepts_removed_markers():
    observed = []

    @bg.compute_node
    def make_set(trigger: bg.TS[int]) -> bg.TSS[int]:
        if trigger.value == 1:
            return {1, 2}
        if trigger.value == 2:
            return {3, bg.Removed(1)}
        return frozenset({4, bg.Removed(2)})

    @bg.compute_node
    def inspect(value: bg.TSS[int]) -> bg.TS[int]:
        observed.append((value.value, value.delta_value))
        return sum(value.value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_set(trigger))

    assert bg.test_graph(graph, [1, 2, 3]) == [3, 5, 7]
    assert observed == [
        (frozenset({1, 2}), bg.SetDelta(added={1, 2})),
        (frozenset({2, 3}), bg.SetDelta(added={3}, removed={1})),
        (frozenset({3, 4}), bg.SetDelta(added={4}, removed={2})),
    ]


def test_removed_marker_matches_hgraph_value_semantics():
    removed = bg.Removed(2)

    assert repr(removed) == "Removed(2)"
    assert hash(removed) == hash(2)
    assert removed == 2
    assert 2 == removed
    assert removed == bg.Removed(2)
    assert removed != bg.Removed(3)


def test_time_series_set_removed_markers_apply_recursively_in_dictionary():
    @bg.compute_node
    def make_dictionary(trigger: bg.TS[int]) -> bg.TSD[int, bg.TSS[int]]:
        if trigger.value == 1:
            return {1: {1, 2}}
        return {1: {3, bg.Removed(1)}}

    @bg.compute_node
    def inspect(value: bg.TSD[int, bg.TSS[int]]) -> bg.TS[int]:
        return sum(value[1].value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_dictionary(trigger))

    assert bg.test_graph(graph, [1, 2]) == [3, 5]


def test_fixed_time_series_list_value_delta_and_item_access():
    observed = []

    @bg.compute_node
    def make_list(trigger: bg.TS[int]) -> bg.TSL[bg.TS[int], 3]:
        if trigger.value == 1:
            return {0: 10, 2: 30}
        return {1: 20}

    @bg.compute_node
    def inspect(value: bg.TSL[bg.TS[int], 3]) -> bg.TS[int]:
        observed.append((value.value, value.delta_value, value[1].valid))
        return sum(item or 0 for item in value.value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_list(trigger))

    assert bg.test_graph(graph, [1, 2]) == [40, 60]
    assert observed == [
        ([10, None, 30], {0: 10, 2: 30}, False),
        ([10, 20, 30], {1: 20}, True),
    ]


def test_time_series_bundle_value_delta_inheritance_and_item_access():
    class Base(bg.TimeSeriesSchema):
        left: bg.TS[int]

    class Pair(Base):
        right: bg.TS[int]

    observed = []

    @bg.compute_node
    def make_bundle(trigger: bg.TS[int]) -> bg.TSB[Pair]:
        if trigger.value == 1:
            return {"left": 10}
        return {"right": 20}

    @bg.compute_node
    def inspect(value: bg.TSB[Pair]) -> bg.TS[int]:
        observed.append((value.value, value.delta_value, value["right"].valid))
        return sum(item or 0 for item in value.value.values())

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_bundle(trigger))

    assert bg.test_graph(graph, [1, 2]) == [10, 30]
    assert observed == [
        ({"left": 10, "right": None}, {"left": 10}, False),
        ({"left": 10, "right": 20}, {"right": 20}, True),
    ]


def test_time_series_schema_rejects_multiple_schema_bases():
    class Left(bg.TimeSeriesSchema):
        left: bg.TS[int]

    class Right(bg.TimeSeriesSchema):
        right: bg.TS[int]

    with pytest.raises(TypeError, match="one schema base"):
        class Invalid(Left, Right):
            pass


def test_time_series_dictionary_value_delta_removal_resurrection_and_item_access():
    observed = []

    @bg.compute_node
    def make_dictionary(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        if trigger.value == 1:
            return {1: 10, 2: 20}
        if trigger.value == 2:
            return {1: bg.REMOVE, 2: 21}
        return {1: 30}

    @bg.compute_node
    def inspect(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        observed.append((value.value, value.delta_value, value[2].value))
        return sum(value.value.values())

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_dictionary(trigger))

    assert bg.test_graph(graph, [1, 2, 3]) == [30, 21, 51]
    assert observed == [
        ({1: 10, 2: 20}, {1: 10, 2: 20}, 20),
        ({2: 21}, {1: bg.REMOVE, 2: 21}, 21),
        ({1: 30, 2: 21}, {1: 30}, 21),
    ]


def test_string_tsd_keys_named_like_delta_fields_are_not_special():
    @bg.compute_node
    def make_dictionary(trigger: bg.TS[int]) -> bg.TSD[str, bg.TS[int]]:
        return {"removed": trigger.value, "modified": trigger.value + 1}

    @bg.compute_node
    def inspect(value: bg.TSD[str, bg.TS[int]]) -> bg.TS[int]:
        return value.value["removed"] + value.value["modified"]

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_dictionary(trigger))

    assert bg.test_graph(graph, [1, 3]) == [3, 7]


def test_nested_collection_deltas_are_applied_recursively():
    @bg.compute_node
    def make_nested(trigger: bg.TS[int]) -> bg.TSD[int, bg.TSL[bg.TS[int], 2]]:
        if trigger.value == 1:
            return {1: {0: 10}}
        return {1: {1: 20}}

    @bg.compute_node
    def read(value: bg.TSD[int, bg.TSL[bg.TS[int], 2]]) -> bg.TS[int]:
        return sum(item or 0 for item in value[1].value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return read(make_nested(trigger))

    assert bg.test_graph(graph, [1, 2]) == [10, 30]


def test_reference_delta_can_be_forwarded_and_dereferenced():
    @bg.compute_node
    def forward(value: bg.REF[bg.TS[int]]) -> bg.REF[bg.TS[int]]:
        assert value.value.valid
        return value.value

    @bg.compute_node
    def read(value: bg.TS[int]) -> bg.TS[int]:
        return value.value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return read(forward(value))

    assert bg.test_graph(graph, [1, 2, 3]) == [1, 2, 3]


def test_reference_to_a_collection_can_be_forwarded_and_dereferenced():
    @bg.compute_node
    def make_dictionary(value: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {1: value.value}

    @bg.compute_node
    def forward(
        value: bg.REF[bg.TSD[int, bg.TS[int]]],
    ) -> bg.REF[bg.TSD[int, bg.TS[int]]]:
        return value.value

    @bg.compute_node
    def read(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return value.value[1]

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return read(forward(make_dictionary(value)))

    assert bg.test_graph(graph, [1, 2]) == [1, 2]


def test_tsd_of_references_notifies_a_dereferenced_python_input():
    @bg.compute_node
    def make_references(
        trigger: bg.TS[int],
        left: bg.REF[bg.TS[int]],
        right: bg.REF[bg.TS[int]],
    ) -> bg.TSD[int, bg.REF[bg.TS[int]]]:
        if trigger.value == 1:
            return {1: left.value, 2: right.value}
        return None

    @bg.compute_node
    def read(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return values.value[1] + values.value[2]

    @bg.graph
    def graph(trigger: bg.TS[int], left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return read(make_references(trigger, left, right))

    assert bg.test_graph(graph, [1, 2, 3], [10, 20, 30], [1, 2, 3]) == [11, 22, 33]


def test_fixed_list_of_references_can_be_dereferenced_as_one_collection():
    @bg.compute_node
    def make_references(
        trigger: bg.TS[int],
        first: bg.REF[bg.TS[int]],
        second: bg.REF[bg.TS[int]],
    ) -> bg.TSL[bg.REF[bg.TS[int]], 2]:
        if trigger.value == 1:
            return {0: first.value, 1: second.value}
        return None

    @bg.graph
    def graph(trigger: bg.TS[int], first: bg.TS[int], second: bg.TS[int]) -> bg.TS[int]:
        values = make_references(trigger, first, second)
        return add(values[0], values[1])

    @bg.compute_node
    def add(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return left.value + right.value

    assert bg.test_graph(graph, [1, 2], [10, 20], [1, 2]) == [11, 22]


def test_bundle_reference_field_can_be_dereferenced():
    class ReferenceBundle(bg.TimeSeriesSchema):
        value: bg.REF[bg.TS[int]]

    @bg.compute_node
    def make_bundle(trigger: bg.TS[int], value: bg.REF[bg.TS[int]]) -> bg.TSB[ReferenceBundle]:
        if trigger.value == 1:
            return {"value": value.value}
        return None

    @bg.compute_node
    def read(value: bg.TS[int]) -> bg.TS[int]:
        return value.value

    @bg.graph
    def graph(trigger: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return read(bg.operators.getattr(make_bundle(trigger, value), bg.symbol("value")))

    assert bg.test_graph(graph, [1, 2], [10, 20]) == [10, 20]


def test_collection_item_and_reference_views_expire_after_the_callback():
    captured_items = []
    captured_references = []

    @bg.compute_node
    def make_list(value: bg.TS[int]) -> bg.TSL[bg.TS[int], 1]:
        return {0: value.value}

    @bg.compute_node
    def inspect(value: bg.TSL[bg.TS[int], 1], reference: bg.REF[bg.TS[int]]) -> bg.TS[int]:
        captured_items.append(value[0])
        captured_references.append(reference.value)
        return value[0].value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_list(value), value)

    assert bg.test_graph(graph, [1]) == [1]
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = captured_items[0].value
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = captured_references[0].valid


def test_collection_delta_validation_reports_the_shape():
    @bg.source_node
    def invalid() -> bg.TSS[int]:
        return [1, 2]

    @bg.compute_node
    def size(value: bg.TSS[int]) -> bg.TS[int]:
        return len(value.value)

    @bg.graph
    def graph() -> bg.TS[int]:
        return size(invalid())

    with pytest.raises(ValueError, match="TSS output requires"):
        bg.test_graph(graph)


def test_set_delta_fields_do_not_interpret_removed_markers():
    @bg.source_node
    def invalid() -> bg.TSS[int]:
        return bg.SetDelta(added={bg.Removed(1)})

    @bg.compute_node
    def size(value: bg.TSS[int]) -> bg.TS[int]:
        return len(value.value)

    @bg.graph
    def graph() -> bg.TS[int]:
        return size(invalid())

    with pytest.raises(TypeError, match="expected an integer"):
        bg.test_graph(graph)
