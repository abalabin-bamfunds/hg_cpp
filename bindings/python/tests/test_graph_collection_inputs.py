from typing import Annotated, TypeVar

import pytest

import bg


def test_valid_items_annotation_is_accepted_and_forwarded_through_python_graphs():
    @bg.compute_node
    def count(values: Annotated[bg.TSD[int, bg.TS[int]], bg.valid_items]) -> bg.TS[int]:
        return len(list(values.valid_items()))

    @bg.graph
    def inner(values: Annotated[bg.TSD[int, bg.TS[int]], bg.valid_items]) -> bg.TS[int]:
        return count(values)

    @bg.graph
    def outer(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return inner(bg.valid_items(values))

    assert bg.test_graph(outer, [{1: 10, 2: 20}, {3: 30}]) == [2, 3]


def test_valid_items_annotation_filters_mapping_operations_without_gating_on_items():
    @bg.compute_node
    def inspect(
        values: Annotated[bg.TSD[int, bg.TS[int]], bg.valid_items], all_valid: bg.TS[bool]
    ) -> bg.TS[tuple[int, tuple[int, ...], bool, bool]]:
        size = len(values)
        keys = tuple(values.keys())
        contains_two = 2 in values
        valid = all_valid.value
        return size, keys, contains_two, valid

    @bg.graph
    def graph(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[tuple[int, tuple[int, ...], bool, bool]]:
        return inspect(bg.valid_items(values), bg.operators.all_valid(values))

    assert bg.test_graph(graph, [{1: 10, 2: 20}, {2: bg.INVALID}, {2: 21}]) == [
        (2, (1, 2), True, True),
        (1, (1,), False, False),
        (2, (1, 2), True, True),
    ]


def test_valid_items_argument_requires_a_valid_items_receiver():
    @bg.compute_node
    def consume(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return len(values.value)

    @bg.graph
    def graph(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return consume(bg.valid_items(values))

    with pytest.raises(ValueError, match="valid_items-qualified argument requires a valid_items input parameter"):
        bg.test_graph(graph, [{1: 10}])


def test_valid_items_metadata_requires_a_tsd_annotation():
    with pytest.raises(ValueError, match="bg.valid_items requires a TSD input annotation"):

        @bg.compute_node
        def invalid(value: Annotated[bg.TS[int], bg.valid_items]) -> bg.TS[int]:
            return value.value


def test_test_graph_tsd_inputs_accept_deltas_and_removals():
    @bg.compute_node
    def total(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return sum(values.value.values())

    @bg.graph
    def graph(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return total(values)

    assert bg.test_graph(
        graph,
        [{1: 10, 2: 20}, {1: bg.REMOVE, 2: 21, 3: 30}, None],
    ) == [30, 51, None]


def test_test_graph_accepts_an_empty_first_tsd_delta():
    @bg.compute_node
    def size(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return len(values.value)

    @bg.graph
    def graph(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return size(values)

    assert bg.test_graph(graph, [{}, {1: 10}]) == [0, 1]


def test_test_graph_tss_inputs_accept_empty_and_incremental_deltas():
    @bg.compute_node
    def total(values: bg.TSS[int]) -> bg.TS[int]:
        return sum(values.value)

    @bg.graph
    def graph(values: bg.TSS[int]) -> bg.TS[int]:
        return total(values)

    assert bg.test_graph(
        graph,
        [set(), {1, 2}, bg.SetDelta(added={3}, removed={1}), bg.SetDelta(removed={2})],
    ) == [0, 3, 5, 3]


def test_test_graph_tss_inputs_accept_removed_markers_in_sets():
    @bg.compute_node
    def total(values: bg.TSS[int]) -> bg.TS[int]:
        return sum(values.value)

    @bg.graph
    def graph(values: bg.TSS[int]) -> bg.TS[int]:
        return total(values)

    assert bg.test_graph(graph, [{1, 2}, {3, bg.Removed(1)}]) == [3, 5]


def test_test_graph_keeps_removed_markers_alive_across_collection_reallocation():
    values = set(range(64))

    @bg.compute_node
    def size(items: bg.TSS[int]) -> bg.TS[int]:
        return len(items.value)

    @bg.graph
    def set_graph(items: bg.TSS[int]) -> bg.TS[int]:
        return size(items)

    assert bg.test_graph(set_graph, [values, bg.SetDelta(removed=values)]) == [64, 0]

    @bg.compute_node
    def nested_size(items: bg.TSD[int, bg.TSS[int]]) -> bg.TS[int]:
        return sum(len(value) for value in items.value.values())

    @bg.graph
    def nested_graph(items: bg.TSD[int, bg.TSS[int]]) -> bg.TS[int]:
        return nested_size(items)

    assert bg.test_graph(
        nested_graph,
        [{1: values}, {1: bg.SetDelta(removed=values)}],
    ) == [64, 0]


def test_test_graph_tsl_inputs_accept_sparse_index_updates():
    @bg.compute_node
    def first(items: bg.TSL[bg.TS[int], 2]) -> bg.TS[int]:
        return items[0].value if items[0].valid else -1

    @bg.graph
    def graph(items: bg.TSL[bg.TS[int], 2]) -> bg.TS[int]:
        return first(items)

    assert bg.test_graph(graph, [{0: 10}, {1: 20}, {0: 30}]) == [10, 10, 30]


def test_test_graph_tsb_inputs_accept_named_field_updates():
    @bg.schema
    class Pair:
        left: bg.TS[int]
        right: bg.TS[int]

    @bg.compute_node
    def total(pair: bg.TSB[Pair]) -> bg.TS[int]:
        left = pair.left.value if pair.left.valid else 0
        right = pair.right.value if pair.right.valid else 0
        return left + right

    @bg.graph
    def graph(pair: bg.TSB[Pair]) -> bg.TS[int]:
        return total(pair)

    assert bg.test_graph(graph, [{"left": 2}, {"right": 5}, {"left": bg.INVALID}]) == [2, 7, 5]


def test_test_graph_converts_nested_collection_inputs_recursively():
    @bg.compute_node
    def total(values: bg.TSD[int, bg.TSS[int]]) -> bg.TS[int]:
        return sum(len(value) for value in values.value.values())

    @bg.graph
    def graph(values: bg.TSD[int, bg.TSS[int]]) -> bg.TS[int]:
        return total(values)

    assert bg.test_graph(graph, [{1: {2, 3}}, {1: bg.SetDelta(added={4}, removed={2})}]) == [2, 2]


def test_test_graph_keeps_scalar_dictionary_ticks_distinct_from_tsd_deltas():
    @bg.graph
    def graph(value: bg.TS[dict[int, str]]) -> bg.TS[dict[int, str]]:
        return value

    result = bg.test_graph(graph, [{1: "first"}, {2: "second"}])
    assert result == [{1: "first"}, {2: "second"}]


def test_test_graph_ts_inputs_accept_invalidation_and_revalidation():
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return value

    assert bg.test_graph(graph, [1, bg.INVALID, 2]) == [1, None, 2]


def test_test_graph_returns_tsd_and_tsl_output_deltas():
    @bg.graph
    def dictionary(values: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return values

    assert bg.test_graph(
        dictionary,
        [{1: 10}, {1: bg.INVALID, 2: 20}, {1: bg.REMOVE}, None],
    ) == [{1: 10}, {1: bg.INVALID, 2: 20}, {1: bg.REMOVE}, None]

    @bg.graph
    def fixed_list(values: bg.TSL[bg.TS[int], 2]) -> bg.TSL[bg.TS[int], 2]:
        return values

    assert bg.test_graph(fixed_list, [{0: 10}, {1: 20}, {1: bg.INVALID}, {0: 11}]) == [
        {0: 10}, {1: 20}, {1: bg.INVALID}, {0: 11}
    ]

    assert bg.test_graph(fixed_list, [bg.INVALID, {0: 10}, bg.INVALID, None, {1: 20}]) == [
        None, {0: 10}, None, None, {1: 20}
    ]


def test_test_graph_returns_tsb_and_nested_collection_deltas():
    @bg.schema
    class Pair:
        left: bg.TS[int]
        right: bg.TS[int]

    @bg.graph
    def bundle(value: bg.TSB[Pair]) -> bg.TSB[Pair]:
        return value

    assert bg.test_graph(bundle, [{"left": 10}, {"right": 20}, {"right": bg.INVALID}, None]) == [
        {"left": 10}, {"right": 20}, {"right": bg.INVALID}, None
    ]
    assert bg.test_graph(bundle, [bg.INVALID, {"left": 10}, bg.INVALID, None, {"right": 20}]) == [
        None, {"left": 10}, None, None, {"right": 20}
    ]

    @bg.graph
    def nested(values: bg.TSD[int, bg.TSB[Pair]]) -> bg.TSD[int, bg.TSB[Pair]]:
        return values

    assert bg.test_graph(
        nested,
        [{1: {"left": 10}}, {1: {"right": 20}}, {1: {"right": bg.INVALID}}, {1: bg.REMOVE}],
    ) == [{1: {"left": 10}}, {1: {"right": 20}}, {1: {"right": bg.INVALID}}, {1: bg.REMOVE}]


def test_test_graph_returns_tss_set_deltas():
    @bg.graph
    def graph(values: bg.TSS[int]) -> bg.TSS[int]:
        return values

    assert bg.test_graph(
        graph,
        [set(), {1, 2}, bg.SetDelta(added={3}, removed={1})],
    ) == [
        bg.SetDelta(),
        bg.SetDelta(added={1, 2}),
        bg.SetDelta(added={3}, removed={1}),
    ]


def test_test_graph_preserves_empty_delta_semantics():
    @bg.graph
    def dictionary(values: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return values

    @bg.graph
    def fixed_list(values: bg.TSL[bg.TS[int], 2]) -> bg.TSL[bg.TS[int], 2]:
        return values

    @bg.schema
    class Pair:
        left: bg.TS[int]

    @bg.graph
    def bundle(value: bg.TSB[Pair]) -> bg.TSB[Pair]:
        return value

    assert bg.test_graph(dictionary, [{}, None]) == [{}, None]
    assert bg.test_graph(fixed_list, [{}, None]) == [None, None]
    assert bg.test_graph(bundle, [{}, None]) == [None, None]


def test_test_graph_decodes_nested_tss_deltas_recursively():
    @bg.graph
    def graph(values: bg.TSD[int, bg.TSL[bg.TSS[int], 1]]) -> bg.TSD[int, bg.TSL[bg.TSS[int], 1]]:
        return values

    assert bg.test_graph(
        graph,
        [{1: {0: {2, 3}}}, {1: {0: bg.SetDelta(added={4}, removed={2})}}],
    ) == [
        {1: {0: bg.SetDelta(added={2, 3})}},
        {1: {0: bg.SetDelta(added={4}, removed={2})}},
    ]


def test_test_graph_converts_string_and_python_object_collection_leaves():
    @bg.graph
    def strings(values: bg.TSD[str, bg.TS[str]]) -> bg.TSD[str, bg.TS[str]]:
        return values

    assert bg.test_graph(strings, [{"first": "one"}, {"second": "two"}]) == [
        {"first": "one"}, {"second": "two"}
    ]

    class Value:
        pass

    value = Value()

    @bg.graph
    def objects(values: bg.TSD[int, bg.TS[Value]]) -> bg.TSD[int, bg.TS[Value]]:
        return values

    result = bg.test_graph(objects, [{1: value}, {2: value}])
    assert result[0][1] is value
    assert result[1][2] is value


def test_test_graph_requires_explicit_specialization_for_generic_collection_inputs():
    KEY = TypeVar("KEY")
    LEFT = TypeVar("LEFT")

    @bg.graph
    def graph(left: bg.TS[LEFT], values: bg.TSD[KEY, bg.TS[str]]) -> bg.TS[str]:
        return values[1]

    specialized = graph[bg.TS[int], bg.TSD[int, bg.TS[str]]]
    assert bg.test_graph(specialized, [0, 0], [{1: "first"}, {1: "second"}]) == ["first", "second"]

    @bg.graph
    def outer(left: bg.TS[int], values: bg.TSD[int, bg.TS[str]]) -> bg.TS[str]:
        return specialized(left, values)

    assert bg.test_graph(outer, [0], [{1: "wired"}]) == ["wired"]
    with pytest.raises(ValueError, match="fully resolved input types"):
        bg.test_graph(graph, [0], [{1: "first"}])
