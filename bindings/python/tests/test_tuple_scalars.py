import subprocess
import sys
from typing import List, Tuple, TypeVar

import pytest

import bg


def test_tuple_scalar_annotation_accepts_builtin_and_typing_forms():
    @bg.graph
    def identity(value: bg.TS[Tuple[int, ...]]) -> bg.TS[tuple[int, ...]]:
        return value

    assert bg.test_graph(identity, [(1, 2), (3,)]) == [(1, 2), (3,)]


def test_tuple_scalar_conversion_accepts_lists_and_returns_immutable_tuples():
    @bg.source_node
    def source() -> bg.TS[tuple[int, ...]]:
        return [1, 2, 3]

    @bg.node
    def inspect(values: tuple[int, ...]) -> int:
        assert type(values) is tuple
        assert values == (1, 2, 3)
        with pytest.raises(TypeError):
            values[0] = 4
        return sum(values)

    @bg.graph
    def graph() -> bg.TS[int]:
        return inspect(source())

    assert bg.test_graph(graph) == [6]


def test_tuple_scalar_output_materializes_as_tuple_in_test_graph():
    @bg.source_node
    def source() -> bg.TS[tuple[int, ...]]:
        return (4, 5)

    @bg.graph
    def graph() -> bg.TS[tuple[int, ...]]:
        return source()

    result = bg.test_graph(graph)
    assert result == [(4, 5)]
    assert type(result[0]) is tuple


def test_tuple_scalar_conversion_copies_a_python_list_during_wiring():
    @bg.node
    def inspect(values: tuple[int, ...]) -> int:
        assert values == (1, 2)
        return len(values)

    @bg.graph
    def graph() -> bg.TS[int]:
        values = [1, 2]
        result = inspect(values)
        values[0] = 99
        return result

    assert bg.test_graph(graph) == [2]


def test_tuple_scalar_conversion_recurses_through_dictionary_values():
    @bg.source_node
    def source() -> bg.TS[dict[str, tuple[int, ...]]]:
        return {"answer": [4, 2]}

    @bg.node
    def inspect(values: dict[str, tuple[int, ...]]) -> int:
        assert values["answer"] == (4, 2)
        assert type(values["answer"]) is tuple
        return sum(values["answer"])

    @bg.graph
    def graph() -> bg.TS[int]:
        return inspect(source())

    assert bg.test_graph(graph) == [6]


def test_tuple_scalar_conversion_recurses_through_nested_tuples():
    @bg.source_node
    def source() -> bg.TS[tuple[tuple[int, ...], ...]]:
        return [[1, 2], (3,)]

    @bg.graph
    def graph() -> bg.TS[tuple[tuple[int, ...], ...]]:
        return source()

    assert bg.test_graph(graph) == [((1, 2), (3,))]


def test_tuple_scalar_items_use_registered_extension_conversions():
    script = """
import bg
from bg._bg_test_extension import register_runtime_double_result_conversion

assert register_runtime_double_result_conversion()

@bg.source_node
def source() -> bg.TS[tuple[float, ...]]:
    return [1.5, 2.5]

@bg.graph
def graph() -> bg.TS[tuple[float, ...]]:
    return source()

assert bg.test_graph(graph) == [("extension scalar conversion", "extension scalar conversion")]
"""
    subprocess.run([sys.executable, "-c", script], check=True, capture_output=True, text=True)


def test_tuple_scalar_values_work_in_tsd_inputs_and_outputs():
    @bg.graph
    def identity(values: bg.TSD[int, bg.TS[tuple[int, ...]]]) -> bg.TSD[int, bg.TS[tuple[int, ...]]]:
        return values

    assert bg.test_graph(identity, [{1: [2, 3]}, {1: (4,), 2: [5, 6]}, None]) == [
        {1: (2, 3)}, {1: (4,), 2: (5, 6)}, None
    ]


def test_tuple_scalar_values_work_in_tsb_inputs_and_outputs():
    @bg.schema
    class Pair:
        values: tuple[int, ...]

    @bg.graph
    def identity(pair: bg.TSB[Pair]) -> bg.TSB[Pair]:
        return pair

    assert bg.test_graph(identity, [{"values": [2, 3]}, {"values": (4,)}, None]) == [
        {"values": (2, 3)}, {"values": (4,)}, None
    ]


def test_tuple_scalar_annotation_resolves_typevars():
    ITEM = TypeVar("ITEM")

    @bg.graph
    def identity(value: bg.TS[tuple[ITEM, ...]]) -> bg.TS[tuple[ITEM, ...]]:
        return value

    @bg.graph
    def graph(value: bg.TS[tuple[int, ...]]) -> bg.TS[tuple[int, ...]]:
        return identity(value)

    assert bg.test_graph(graph, [(8, 13)]) == [(8, 13)]


def test_generic_python_node_resolves_tuple_item_type():
    ITEM = TypeVar("ITEM")

    @bg.compute_node
    def identity(value: bg.TS[tuple[ITEM, ...]]) -> bg.TS[tuple[ITEM, ...]]:
        return value.value

    @bg.graph
    def graph(value: bg.TS[tuple[int, ...]]) -> bg.TS[tuple[int, ...]]:
        return identity(value)

    assert bg.test_graph(graph, [(3, 5, 8)]) == [(3, 5, 8)]


@pytest.mark.parametrize("annotation", [tuple, Tuple, tuple[int], tuple[int, str]])
def test_fixed_or_unparameterized_tuple_annotations_are_rejected(annotation):
    with pytest.raises(TypeError, match=r"tuple scalar annotations must use tuple\[T, \.\.\.\]"):
        bg._native_type(annotation)


@pytest.mark.parametrize("annotation", [list, List, list[int], List[int]])
def test_mutable_list_annotations_are_rejected(annotation):
    with pytest.raises(TypeError, match="list annotations are mutable"):
        bg._native_type(annotation)


def test_tuple_scalar_annotation_rejects_time_series_items():
    with pytest.raises(TypeError, match=r"tuple\[T, \.\.\.\] requires a scalar item annotation"):
        bg._native_type(tuple[bg.TS[int], ...])


def test_tuple_scalar_annotation_rejects_optional_items():
    with pytest.raises(TypeError, match=r"tuple\[T, \.\.\.\] does not support optional items"):
        bg._native_type(tuple[int | None, ...])


def test_tuple_scalar_conversion_rejects_invalid_item_values():
    @bg.source_node
    def source() -> bg.TS[tuple[int, ...]]:
        return ["not an integer"]

    @bg.graph
    def graph() -> bg.TS[tuple[int, ...]]:
        return source()

    with pytest.raises(TypeError):
        bg.test_graph(graph)


def test_tuple_scalar_conversion_releases_items_when_a_later_item_fails():
    class Base:
        pass

    class Derived(Base):
        pass

    class Unrelated:
        pass

    first = Derived()

    @bg.node
    def consume(values: tuple[Base, ...]) -> int:
        return len(values)

    @bg.graph
    def graph() -> bg.TS[int]:
        return consume([first, Unrelated()])

    reference_count = sys.getrefcount(first)
    with pytest.raises(TypeError, match="not an instance"):
        bg.test_graph(graph)
    assert sys.getrefcount(first) == reference_count


def test_empty_tuple_scalar_round_trips():
    @bg.source_node
    def source() -> bg.TS[tuple[int, ...]]:
        return ()

    @bg.graph
    def graph() -> bg.TS[tuple[int, ...]]:
        return source()

    assert bg.test_graph(graph) == [()]
