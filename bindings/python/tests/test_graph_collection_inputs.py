from typing import TypeVar

import pytest

import bg


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
