from datetime import timedelta
from types import MappingProxyType
from typing import Annotated, TypeVar

import pytest

import hgraph as hg


def test_run_graph_tsd_inputs_accept_deltas_and_removals():
    @hg.compute_node
    def total(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return sum(values.value.values())

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return total(values)

    assert hg.run_graph(
        graph,
        [{1: 10, 2: 20}, {1: hg.REMOVE, 2: 21, 3: 30}, None],
    ) == [30, 51, None]


def test_run_graph_tsl_inputs_accept_sparse_index_updates():
    @hg.compute_node
    def first(items: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return items[0].value if items[0].valid else -1

    @hg.graph
    def graph(items: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return first(items)

    assert hg.run_graph(graph, [{0: 10}, {1: 20}, {0: 30}]) == [10, 10, 30]


def test_run_graph_tsb_inputs_accept_named_field_updates():
    @hg.schema
    class Pair:
        left: hg.TS[int]
        right: hg.TS[int]

    @hg.compute_node
    def total(pair: hg.TSB[Pair]) -> hg.TS[int]:
        left = pair.left.value if pair.left.valid else 0
        right = pair.right.value if pair.right.valid else 0
        return left + right

    @hg.graph
    def graph(pair: hg.TSB[Pair]) -> hg.TS[int]:
        return total(pair)

    assert hg.run_graph(graph, [{"left": 2}, {"right": 5}, {"left": hg.INVALID}]) == [2, 7, 5]


def test_run_graph_tss_inputs_accept_empty_and_incremental_deltas():
    @hg.compute_node
    def total(values: hg.TSS[int]) -> hg.TS[int]:
        return sum(values.value)

    @hg.graph
    def graph(values: hg.TSS[int]) -> hg.TS[int]:
        return total(values)

    assert hg.run_graph(
        graph,
        [set(), {1, 2}, hg.SetDelta(added={3}, removed={1}), hg.SetDelta(removed={2})],
    ) == [0, 3, 5, 3]


def test_run_graph_tss_inputs_accept_removed_markers_in_sets():
    @hg.compute_node
    def total(values: hg.TSS[int]) -> hg.TS[int]:
        return sum(values.value)

    @hg.graph
    def graph(values: hg.TSS[int]) -> hg.TS[int]:
        return total(values)

    assert hg.run_graph(graph, [{1, 2}, {3, hg.Removed(1)}]) == [3, 5]


def test_run_graph_returns_tsd_and_tsl_output_deltas():
    @hg.graph
    def dictionary(values: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return values

    assert hg.run_graph(
        dictionary,
        [{1: 10}, {1: hg.INVALID, 2: 20}, {1: hg.REMOVE}, None],
    ) == [{1: 10}, {1: hg.INVALID, 2: 20}, {1: hg.REMOVE}, None]

    @hg.graph
    def fixed_list(values: hg.TSL[hg.TS[int], 2]) -> hg.TSL[hg.TS[int], 2]:
        return values

    assert hg.run_graph(fixed_list, [{0: 10}, {1: 20}, {1: hg.INVALID}, {0: 11}]) == [
        {0: 10}, {1: 20}, {1: hg.INVALID}, {0: 11}
    ]

    assert hg.run_graph(fixed_list, [hg.INVALID, {0: 10}, hg.INVALID, None, {1: 20}]) == [
        None, {0: 10}, None, None, {1: 20}
    ]


def test_run_graph_returns_tsb_and_nested_collection_deltas():
    @hg.schema
    class Pair:
        left: hg.TS[int]
        right: hg.TS[int]

    @hg.graph
    def bundle(value: hg.TSB[Pair]) -> hg.TSB[Pair]:
        return value

    assert hg.run_graph(bundle, [{"left": 10}, {"right": 20}, {"right": hg.INVALID}, None]) == [
        {"left": 10}, {"right": 20}, {"right": hg.INVALID}, None
    ]
    assert hg.run_graph(bundle, [hg.INVALID, {"left": 10}, hg.INVALID, None, {"right": 20}]) == [
        None, {"left": 10}, None, None, {"right": 20}
    ]

    @hg.graph
    def nested(values: hg.TSD[int, hg.TSB[Pair]]) -> hg.TSD[int, hg.TSB[Pair]]:
        return values

    assert hg.run_graph(
        nested,
        [{1: {"left": 10}}, {1: {"right": 20}}, {1: {"right": hg.INVALID}}, {1: hg.REMOVE}],
    ) == [{1: {"left": 10}}, {1: {"right": 20}}, {1: {"right": hg.INVALID}}, {1: hg.REMOVE}]


def test_run_graph_returns_tss_set_deltas():
    @hg.graph
    def graph(values: hg.TSS[int]) -> hg.TSS[int]:
        return values

    assert hg.run_graph(
        graph,
        [set(), {1, 2}, hg.SetDelta(added={3}, removed={1})],
    ) == [
        hg.SetDelta(),
        hg.SetDelta(added={1, 2}),
        hg.SetDelta(added={3}, removed={1}),
    ]


def test_valid_items_annotation_is_accepted_and_forwarded_through_python_graphs():
    @hg.compute_node
    def count(values: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items]) -> hg.TS[int]:
        return len(list(values.valid_items()))

    @hg.graph
    def inner(values: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items]) -> hg.TS[int]:
        return count(values)

    @hg.graph
    def outer(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return inner(hg.valid_items(values))

    assert hg.run_graph(outer, [{1: 10, 2: 20}, {3: 30}]) == [2, 3]


def test_valid_items_annotation_filters_mapping_operations_without_gating_on_items():
    @hg.compute_node
    def inspect(
        values: Annotated[hg.TSD[int, hg.TS[int]], hg.valid_items], all_valid: hg.TS[bool]
    ) -> hg.TS[tuple[int, tuple[int, ...], bool, bool]]:
        size = len(values)
        keys = tuple(values.keys())
        contains_two = 2 in values
        valid = all_valid.value
        return size, keys, contains_two, valid

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[tuple[int, tuple[int, ...], bool, bool]]:
        return inspect(hg.valid_items(values), hg.operators.all_valid(values))

    assert hg.run_graph(graph, [{1: 10, 2: 20}, {2: hg.INVALID}, {2: 21}]) == [
        (2, (1, 2), True, True),
        (1, (1,), False, False),
        (2, (1, 2), True, True),
    ]


def test_valid_items_argument_requires_a_valid_items_receiver():
    @hg.compute_node
    def consume(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return len(values.value)

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return consume(hg.valid_items(values))

    with pytest.raises(ValueError, match="valid_items-qualified argument requires a valid_items input parameter"):
        hg.run_graph(graph, [{1: 10}])


def test_valid_items_metadata_requires_a_tsd_annotation():
    with pytest.raises(ValueError, match="hgraph.valid_items requires a TSD input annotation"):

        @hg.compute_node
        def invalid(value: Annotated[hg.TS[int], hg.valid_items]) -> hg.TS[int]:
            return value.value


def test_run_graph_accepts_mapping_protocol_values_and_collection_deltas():
    @hg.graph
    def consume_const(value: hg.TS[dict[int, str], hg.const_]) -> hg.TS[dict[int, str]]:
        return value

    @hg.graph
    def scalar_dictionary(value: hg.TS[dict[int, str]]) -> hg.TS[dict[int, str]]:
        return consume_const(value)

    @hg.graph
    def dictionary(values: hg.TSD[int, hg.TS[str]]) -> hg.TSD[int, hg.TS[str]]:
        return values

    @hg.graph
    def list_(values: hg.TSL[hg.TS[int], 2]) -> hg.TSL[hg.TS[int], 2]:
        return values

    @hg.schema
    class Pair:
        left: hg.TS[int]
        right: hg.TS[int]

    @hg.graph
    def bundle(values: hg.TSB[Pair]) -> hg.TSB[Pair]:
        return values

    assert hg.run_graph(scalar_dictionary, MappingProxyType({1: "scalar"})) == [{1: "scalar"}]
    assert hg.run_graph(dictionary, MappingProxyType({1: "dictionary"})) == [{1: "dictionary"}]
    assert hg.run_graph(list_, MappingProxyType({1: 2})) == [{1: 2}]
    assert hg.run_graph(bundle, MappingProxyType({"left": 3})) == [{"left": 3}]


def test_run_graph_ticks_disambiguates_a_list_valued_scalar_and_accepts_empty_history():
    @hg.graph
    def identity(value: hg.TS[tuple[int, ...]]) -> hg.TS[tuple[int, ...]]:
        return value

    assert hg.run_graph(identity, [1, 2]) == [(1, 2)]
    assert hg.run_graph(identity, hg.ticks([])) == [None]


def test_run_graph_scalar_inputs_keep_the_const_guarantee_of_normal_wiring():
    @hg.graph
    def consume_const(value: hg.TS[int, hg.const_]) -> hg.TS[int]:
        return value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return consume_const(value)

    assert hg.run_graph(graph, 5) == [5]


def test_run_graph_rejects_a_history_for_a_root_const_input():
    @hg.graph
    def identity(value: hg.TS[int, hg.const_]) -> hg.TS[int]:
        return value

    with pytest.raises(ValueError):
        hg.run_graph(identity, [1, 2])


def test_run_graph_rejects_a_history_forwarded_to_a_nested_const_input():
    @hg.graph
    def consume_const(value: hg.TS[int, hg.const_]) -> hg.TS[int]:
        return value

    @hg.graph
    def outer(value: hg.TS[int]) -> hg.TS[int]:
        return consume_const(value)

    with pytest.raises(ValueError):
        hg.run_graph(outer, [1, 2])


def test_run_graph_preserves_the_empty_history_baseline_and_empty_collection_event():
    @hg.graph
    def scalar_identity(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def collection_identity(value: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return value

    assert hg.run_graph(scalar_identity, []) == [None]
    assert hg.run_graph(collection_identity, [{}]) == [{}]


def test_run_graph_keeps_input_length_after_trailing_invalid_output_ticks():
    @hg.graph
    def identity(value: hg.TS[int]) -> hg.TS[int]:
        return value

    result = hg.run_graph(identity, [1, hg.INVALID, hg.INVALID])

    assert result == [1, None, None]
    assert len(result) == 3
    assert result.event_count == 2
    assert result.events[0] == (result.start_time, 1)
    assert result.events[1][0] > result.start_time
    assert result.events[1][1] is None


def test_run_graph_result_bounds_repr_and_compares_long_dense_values():
    @hg.graph
    def identity(value: hg.TS[int]) -> hg.TS[int]:
        return value

    values = list(range(40))
    result = hg.run_graph(identity, values)

    assert result == values
    assert result != values[:-1]
    assert "size=40" in repr(result)
    assert "event_count=40" in repr(result)


def test_run_graph_keeps_a_sparse_event_after_a_large_simulation_gap():
    @hg.generator
    def source(clock: hg.EvaluationClock = None) -> hg.TS[int]:
        yield clock.evaluation_time + timedelta(hours=1), 7

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    result = hg.run_graph(graph)

    assert result.event_count == 1
    assert len(result) == 3_600_000_001
    assert result[0] is None
    assert result[-1] == 7
    assert result != [7]
    assert len(repr(result)) < 200


def test_run_graph_classifies_tuple_and_schema_values_by_their_types():
    @hg.schema
    class Quote:
        bid: int
        ask: int

    @hg.graph
    def tuple_identity(value: hg.TS[tuple[int, int]]) -> hg.TS[tuple[int, int]]:
        return value

    @hg.graph
    def schema_identity(value: hg.TS[hg.Bundle[Quote]]) -> hg.TS[hg.Bundle[Quote]]:
        return value

    quote = Quote(2, 3)
    assert hg.run_graph(tuple_identity, [(1, 2), (3, 4)]) == [(1, 2), (3, 4)]
    assert hg.run_graph(tuple_identity, (5, 6)) == [(5, 6)]
    assert hg.run_graph(tuple_identity, [7, 8]) == [(7, 8)]
    result = hg.run_graph(schema_identity, [quote])
    assert [(item.bid, item.ask) for item in result] == [(2, 3)]


def test_run_graph_classifies_nested_empty_collections_as_values_or_ticks():
    @hg.graph
    def identity(value: hg.TS[tuple[tuple[int, ...], ...]]) -> hg.TS[tuple[tuple[int, ...], ...]]:
        return value

    assert hg.run_graph(identity, [[], []]) == [((), ())]
    assert hg.run_graph(identity, hg.ticks([[], []])) == [(), ()]


def test_run_graph_accepts_an_empty_first_tsd_delta():
    @hg.compute_node
    def size(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return len(values.value)

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return size(values)

    assert hg.run_graph(graph, [{}, {1: 10}]) == [0, 1]


def test_run_graph_keeps_removed_markers_alive_across_collection_reallocation():
    values = set(range(64))

    @hg.compute_node
    def size(items: hg.TSS[int]) -> hg.TS[int]:
        return len(items.value)

    @hg.graph
    def set_graph(items: hg.TSS[int]) -> hg.TS[int]:
        return size(items)

    assert hg.run_graph(set_graph, [values, hg.SetDelta(removed=values)]) == [64, 0]

    @hg.compute_node
    def nested_size(items: hg.TSD[int, hg.TSS[int]]) -> hg.TS[int]:
        return sum(len(value) for value in items.value.values())

    @hg.graph
    def nested_graph(items: hg.TSD[int, hg.TSS[int]]) -> hg.TS[int]:
        return nested_size(items)

    assert hg.run_graph(
        nested_graph,
        [{1: values}, {1: hg.SetDelta(removed=values)}],
    ) == [64, 0]


def test_run_graph_converts_nested_collection_inputs_recursively():
    @hg.compute_node
    def total(values: hg.TSD[int, hg.TSS[int]]) -> hg.TS[int]:
        return sum(len(value) for value in values.value.values())

    @hg.graph
    def graph(values: hg.TSD[int, hg.TSS[int]]) -> hg.TS[int]:
        return total(values)

    assert hg.run_graph(graph, [{1: {2, 3}}, {1: hg.SetDelta(added={4}, removed={2})}]) == [2, 2]


def test_run_graph_keeps_scalar_dictionary_ticks_distinct_from_tsd_deltas():
    @hg.graph
    def graph(value: hg.TS[dict[int, str]]) -> hg.TS[dict[int, str]]:
        return value

    result = hg.run_graph(graph, [{1: "first"}, {2: "second"}])
    assert result == [{1: "first"}, {2: "second"}]


def test_run_graph_ts_inputs_accept_invalidation_and_revalidation():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return value

    assert hg.run_graph(graph, [1, hg.INVALID, 2]) == [1, None, 2]


def test_run_graph_preserves_empty_delta_semantics():
    @hg.graph
    def dictionary(values: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return values

    @hg.graph
    def fixed_list(values: hg.TSL[hg.TS[int], 2]) -> hg.TSL[hg.TS[int], 2]:
        return values

    @hg.schema
    class Pair:
        left: hg.TS[int]

    @hg.graph
    def bundle(value: hg.TSB[Pair]) -> hg.TSB[Pair]:
        return value

    assert hg.run_graph(dictionary, [{}, None]) == [{}, None]
    assert hg.run_graph(fixed_list, [{}, None]) == [None, None]
    assert hg.run_graph(bundle, [{}, None]) == [None, None]


def test_run_graph_decodes_nested_tss_deltas_recursively():
    @hg.graph
    def graph(values: hg.TSD[int, hg.TSL[hg.TSS[int], 1]]) -> hg.TSD[int, hg.TSL[hg.TSS[int], 1]]:
        return values

    assert hg.run_graph(
        graph,
        [{1: {0: {2, 3}}}, {1: {0: hg.SetDelta(added={4}, removed={2})}}],
    ) == [
        {1: {0: hg.SetDelta(added={2, 3})}},
        {1: {0: hg.SetDelta(added={4}, removed={2})}},
    ]


def test_run_graph_converts_string_and_python_object_collection_leaves():
    @hg.graph
    def strings(values: hg.TSD[str, hg.TS[str]]) -> hg.TSD[str, hg.TS[str]]:
        return values

    assert hg.run_graph(strings, [{"first": "one"}, {"second": "two"}]) == [
        {"first": "one"}, {"second": "two"}
    ]

    class Value:
        pass

    value = Value()

    @hg.graph
    def objects(values: hg.TSD[int, hg.TS[Value]]) -> hg.TSD[int, hg.TS[Value]]:
        return values

    result = hg.run_graph(objects, [{1: value}, {2: value}])
    assert result[0][1] is value
    assert result[1][2] is value


def test_run_graph_requires_explicit_specialization_for_generic_collection_inputs():
    KEY = TypeVar("KEY")
    LEFT = TypeVar("LEFT")

    @hg.graph
    def graph(left: hg.TS[LEFT], values: hg.TSD[KEY, hg.TS[str]]) -> hg.TS[str]:
        return values[1]

    specialized = graph[hg.TS[int], hg.TSD[int, hg.TS[str]]]
    assert hg.run_graph(specialized, [0, 0], [{1: "first"}, {1: "second"}]) == ["first", "second"]

    @hg.graph
    def outer(left: hg.TS[int], values: hg.TSD[int, hg.TS[str]]) -> hg.TS[str]:
        return specialized(left, values)

    assert hg.run_graph(outer, [0], [{1: "wired"}]) == ["wired"]
    with pytest.raises(ValueError, match="fully resolved input types"):
        hg.run_graph(graph, [0], [{1: "first"}])
