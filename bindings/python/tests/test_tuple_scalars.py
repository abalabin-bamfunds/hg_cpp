import subprocess
import sys
from typing import List, Tuple, TypeVar

import pytest

import hgraph as hg


def test_tuple_scalar_annotation_accepts_builtin_and_typing_forms():
    @hg.graph
    def identity(value: hg.TS[Tuple[int, ...]]) -> hg.TS[tuple[int, ...]]:
        return value

    assert hg.run_graph(identity, [(1, 2), (3,)]) == [(1, 2), (3,)]


def test_fixed_tuple_scalar_annotations_convert_each_position():
    @hg.graph
    def identity(value: hg.TS[tuple[str, int, hg.Symbol]]) -> hg.TS[Tuple[str, int, hg.Symbol]]:
        return value

    assert hg.run_graph(identity, [["answer", 42, hg.Symbol("ok")]]) == [("answer", 42, hg.Symbol("ok"))]


def test_fixed_tuple_scalar_supports_single_and_empty_tuples():
    @hg.graph
    def single(value: hg.TS[tuple[int]]) -> hg.TS[tuple[int]]:
        return value

    @hg.graph
    def empty(value: hg.TS[tuple[()]]) -> hg.TS[Tuple[()]]:
        return value

    assert hg.run_graph(single, [[7]]) == [(7,)]
    assert hg.run_graph(empty, [[]]) == [()]


def test_fixed_tuple_scalar_converts_typing_tuple_annotations():
    @hg.graph
    def identity(value: hg.TS[Tuple[str, int]]) -> hg.TS[tuple[str, int]]:
        return value

    assert hg.run_graph(identity, [("life", 42)]) == [("life", 42)]


def test_tuple_scalar_conversion_accepts_lists_and_returns_immutable_tuples():
    @hg.source_node
    def source() -> hg.TS[tuple[int, ...]]:
        return [1, 2, 3]

    @hg.node
    def inspect(values: tuple[int, ...]) -> int:
        assert type(values) is tuple
        assert values == (1, 2, 3)
        with pytest.raises(TypeError):
            values[0] = 4
        return sum(values)

    @hg.graph
    def graph() -> hg.TS[int]:
        return inspect(source())

    assert hg.run_graph(graph) == [6]


def test_tuple_scalar_output_materializes_as_tuple_in_run_graph():
    @hg.source_node
    def source() -> hg.TS[tuple[int, ...]]:
        return (4, 5)

    @hg.graph
    def graph() -> hg.TS[tuple[int, ...]]:
        return source()

    result = hg.run_graph(graph)
    assert result == [(4, 5)]
    assert type(result[0]) is tuple


def test_tuple_scalar_conversion_copies_a_python_list_during_wiring():
    @hg.node
    def inspect(values: tuple[int, ...]) -> int:
        assert values == (1, 2)
        return len(values)

    @hg.graph
    def graph() -> hg.TS[int]:
        values = [1, 2]
        result = inspect(values)
        values[0] = 99
        return result

    assert hg.run_graph(graph) == [2]


def test_fixed_tuple_scalar_conversion_copies_a_python_list_during_wiring():
    @hg.node
    def inspect(values: tuple[int, str]) -> int:
        assert values == (1, "before")
        return len(values)

    @hg.graph
    def graph() -> hg.TS[int]:
        values = [1, "before"]
        result = inspect(values)
        values[0] = 99
        values[1] = "after"
        return result

    assert hg.run_graph(graph) == [2]


def test_tuple_scalar_conversion_recurses_through_dictionary_values():
    @hg.source_node
    def source() -> hg.TS[dict[str, tuple[int, ...]]]:
        return {"answer": [4, 2]}

    @hg.node
    def inspect(values: dict[str, tuple[int, ...]]) -> int:
        assert values["answer"] == (4, 2)
        assert type(values["answer"]) is tuple
        return sum(values["answer"])

    @hg.graph
    def graph() -> hg.TS[int]:
        return inspect(source())

    assert hg.run_graph(graph) == [6]


def test_tuple_scalar_conversion_recurses_through_nested_tuples():
    @hg.source_node
    def source() -> hg.TS[tuple[tuple[int, ...], ...]]:
        return [[1, 2], (3,)]

    @hg.graph
    def graph() -> hg.TS[tuple[tuple[int, ...], ...]]:
        return source()

    assert hg.run_graph(graph) == [((1, 2), (3,))]


def test_fixed_tuple_scalar_conversion_recurses_through_nested_fixed_tuples():
    @hg.source_node
    def source() -> hg.TS[tuple[tuple[int, str], tuple[bool]]]:
        return [[1, "one"], [True]]

    @hg.graph
    def graph() -> hg.TS[tuple[tuple[int, str], tuple[bool]]]:
        return source()

    assert hg.run_graph(graph) == [((1, "one"), (True,))]


def test_nested_tuple_collection_input_outlives_temporary_conversion_storage():
    @hg.node
    def total(values: tuple[tuple[int, ...], dict[str, tuple[int, ...]]]) -> int:
        return sum(values[0]) + sum(values[1]["items"])

    @hg.graph
    def graph() -> hg.TS[int]:
        return total(([1, 2], {"items": [3, 4]}))

    assert hg.run_graph(graph) == [10]


def test_generic_compute_node_resolves_fixed_and_homogeneous_nested_tuples():
    ITEM = TypeVar("ITEM")

    @hg.compute_node
    def identity(value: hg.TS[tuple[tuple[ITEM, ...], tuple[ITEM, str]]]) -> hg.TS[
        tuple[tuple[ITEM, ...], tuple[ITEM, str]]
    ]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[tuple[tuple[int, ...], tuple[int, str]]]) -> hg.TS[
        tuple[tuple[int, ...], tuple[int, str]]
    ]:
        return identity(value)

    assert hg.run_graph(graph, [([1, 2], [3, "three"])]) == [((1, 2), (3, "three"))]


def test_fixed_tuple_scalar_conversion_recurses_through_dictionary_keys_and_values():
    @hg.graph
    def identity(value: hg.TS[dict[tuple[int, str], tuple[bool, int]]]) -> hg.TS[
        dict[tuple[int, str], tuple[bool, int]]
    ]:
        return value

    assert hg.run_graph(identity, [{(1, "one"): [True, 2]}]) == [{(1, "one"): (True, 2)}]


def test_tuple_scalar_items_use_registered_extension_conversions():
    script = """
import hgraph as hg
from hgraph._hg_test_extension import register_runtime_double_result_conversion

assert register_runtime_double_result_conversion()

@hg.source_node
def source() -> hg.TS[tuple[float, ...]]:
    return [1.5, 2.5]

@hg.graph
def graph() -> hg.TS[tuple[float, ...]]:
    return source()

assert hg.run_graph(graph) == [("extension scalar conversion", "extension scalar conversion")]
"""
    subprocess.run([sys.executable, "-c", script], check=True, capture_output=True, text=True)


def test_fixed_tuple_items_use_registered_extension_conversions():
    script = """
import hgraph as hg
from hgraph._hg_test_extension import register_runtime_double_result_conversion

assert register_runtime_double_result_conversion()

@hg.source_node
def source() -> hg.TS[tuple[float, str]]:
    return [1.5, "value"]

@hg.graph
def graph() -> hg.TS[tuple[float, str]]:
    return source()

assert hg.run_graph(graph) == [("extension scalar conversion", "value")]
"""
    subprocess.run([sys.executable, "-c", script], check=True, capture_output=True, text=True)


def test_tuple_scalar_values_work_in_tsd_inputs_and_outputs():
    @hg.graph
    def identity(values: hg.TSD[int, hg.TS[tuple[int, ...]]]) -> hg.TSD[int, hg.TS[tuple[int, ...]]]:
        return values

    assert hg.run_graph(identity, [{1: [2, 3]}, {1: (4,), 2: [5, 6]}, None]) == [
        {1: (2, 3)}, {1: (4,), 2: (5, 6)}, None
    ]


def test_tuple_scalar_values_work_in_tsb_inputs_and_outputs():
    @hg.schema
    class Pair:
        values: tuple[int, ...]

    @hg.graph
    def identity(pair: hg.TSB[Pair]) -> hg.TSB[Pair]:
        return pair

    assert hg.run_graph(identity, [{"values": [2, 3]}, {"values": (4,)}, None]) == [
        {"values": (2, 3)}, {"values": (4,)}, None
    ]


def test_tuple_scalar_annotation_resolves_typevars():
    ITEM = TypeVar("ITEM")

    @hg.graph
    def identity(value: hg.TS[tuple[ITEM, ...]]) -> hg.TS[tuple[ITEM, ...]]:
        return value

    @hg.graph
    def graph(value: hg.TS[tuple[int, ...]]) -> hg.TS[tuple[int, ...]]:
        return identity(value)

    assert hg.run_graph(graph, [(8, 13)]) == [(8, 13)]


def test_fixed_tuple_scalar_annotation_resolves_typevars():
    ITEM = TypeVar("ITEM")

    @hg.graph
    def identity(value: hg.TS[tuple[ITEM, str]]) -> hg.TS[tuple[ITEM, str]]:
        return value

    @hg.graph
    def graph(value: hg.TS[tuple[int, str]]) -> hg.TS[tuple[int, str]]:
        return identity(value)

    assert hg.run_graph(graph, [(8, "eight")]) == [(8, "eight")]


def test_generic_python_node_resolves_tuple_item_type():
    ITEM = TypeVar("ITEM")

    @hg.compute_node
    def identity(value: hg.TS[tuple[ITEM, ...]]) -> hg.TS[tuple[ITEM, ...]]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[tuple[int, ...]]) -> hg.TS[tuple[int, ...]]:
        return identity(value)

    assert hg.run_graph(graph, [(3, 5, 8)]) == [(3, 5, 8)]


@pytest.mark.parametrize("annotation", [tuple, Tuple])
def test_unparameterized_tuple_annotations_are_rejected(annotation):
    with pytest.raises(TypeError, match="tuple annotations must specify item types"):
        hg._native_type(annotation)


@pytest.mark.parametrize("annotation", [tuple[int, ...], tuple[int, str], tuple[()]])
def test_fixed_tuple_annotations_resolve_to_scalar_types(annotation):
    assert not hg._native_type(annotation).is_time_series


@pytest.mark.parametrize("annotation", [list, List, list[int], List[int]])
def test_mutable_list_annotations_are_rejected(annotation):
    with pytest.raises(TypeError, match="list annotations are mutable"):
        hg._native_type(annotation)


def test_tuple_scalar_annotation_rejects_time_series_items():
    with pytest.raises(TypeError, match=r"list\[T\] requires a scalar item annotation"):
        hg._native_type(tuple[hg.TS[int], ...])


def test_fixed_tuple_scalar_annotation_rejects_time_series_items():
    with pytest.raises(TypeError, match="fixed tuple annotations require scalar item annotations"):
        hg._native_type(tuple[hg.TS[int], str])


def test_tuple_scalar_annotation_rejects_optional_items():
    with pytest.raises(TypeError, match=r"Optional\[\.\.\.\] cannot be used as a list item type"):
        hg._native_type(tuple[int | None, ...])


def test_fixed_tuple_scalar_annotation_rejects_optional_items():
    with pytest.raises(TypeError, match="fixed tuple annotations do not support optional items"):
        hg._native_type(tuple[int | None, str])


def test_fixed_tuple_scalar_annotation_rejects_malformed_ellipsis():
    with pytest.raises(TypeError, match="unsupported Python node annotation"):
        hg._native_type(tuple[int, ..., str])


def test_tuple_scalar_conversion_rejects_invalid_item_values():
    @hg.source_node
    def source() -> hg.TS[tuple[int, ...]]:
        return ["not an integer"]

    @hg.graph
    def graph() -> hg.TS[tuple[int, ...]]:
        return source()

    with pytest.raises(TypeError):
        hg.run_graph(graph)


@pytest.mark.parametrize("value", [(1,), (1, 2, 3)])
def test_fixed_tuple_scalar_conversion_requires_exact_arity(value):
    @hg.source_node
    def source() -> hg.TS[tuple[int, int]]:
        return value

    @hg.graph
    def graph() -> hg.TS[tuple[int, int]]:
        return source()

    with pytest.raises(TypeError, match="expected a fixed tuple scalar with 2 items"):
        hg.run_graph(graph)


def test_fixed_tuple_scalar_conversion_checks_items_after_reentrant_list_mutation():
    values = []

    class MutatingFloat:
        def __float__(self):
            values.clear()
            return 1.5

    values.extend([MutatingFloat(), 2.5])

    @hg.node
    def consume(items: tuple[float, float]) -> float:
        return sum(items)

    @hg.graph
    def graph() -> hg.TS[float]:
        return consume(values)

    with pytest.raises(IndexError, match="list index out of range"):
        hg.run_graph(graph)


def test_tuple_scalar_conversion_releases_items_when_a_later_item_fails():
    class Base:
        pass

    class Derived(Base):
        pass

    class Unrelated:
        pass

    first = Derived()

    @hg.node
    def consume(values: tuple[Base, ...]) -> int:
        return len(values)

    @hg.graph
    def graph() -> hg.TS[int]:
        return consume([first, Unrelated()])

    reference_count = sys.getrefcount(first)
    with pytest.raises(TypeError, match="not an instance"):
        hg.run_graph(graph)
    assert sys.getrefcount(first) == reference_count


def test_fixed_tuple_scalar_conversion_releases_items_when_a_later_item_fails():
    class Base:
        pass

    class Derived(Base):
        pass

    class Unrelated:
        pass

    first = Derived()

    @hg.node
    def consume(values: tuple[Base, Base]) -> int:
        return len(values)

    @hg.graph
    def graph() -> hg.TS[int]:
        return consume([first, Unrelated()])

    reference_count = sys.getrefcount(first)
    with pytest.raises(TypeError, match="not an instance"):
        hg.run_graph(graph)
    assert sys.getrefcount(first) == reference_count


def test_empty_tuple_scalar_round_trips():
    @hg.source_node
    def source() -> hg.TS[tuple[int, ...]]:
        return ()

    @hg.graph
    def graph() -> hg.TS[tuple[int, ...]]:
        return source()

    assert hg.run_graph(graph) == [()]
