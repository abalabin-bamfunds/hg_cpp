import subprocess
import sys
from typing import Optional, TypeVar

import pytest

import bg


def test_scalar_type_variable_in_time_series_input_and_output():
    VALUE = TypeVar("VALUE")

    @bg.graph
    def identity(value: bg.TS[VALUE]) -> bg.TS[VALUE]:
        return value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return identity(value)

    assert bg.test_graph(graph, [2, 5]) == [2, 5]


def test_scalar_type_variable_in_plain_graph_input_and_output():
    VALUE = TypeVar("VALUE")

    @bg.graph
    def identity(value: VALUE) -> VALUE:
        return value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return identity(value)

    assert bg.test_graph(graph, [3, 7]) == [3, 7]


def test_scalar_type_variable_in_compute_node_input_and_output():
    VALUE = TypeVar("VALUE")

    @bg.compute_node
    def increment(value: bg.TS[VALUE]) -> bg.TS[VALUE]:
        return value.value + 1

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return increment(value)

    assert bg.test_graph(graph, [1, 4]) == [2, 5]


def test_time_series_type_variable_forwarding():
    SERIES = TypeVar("SERIES", bound=bg.TimeSeries)

    @bg.graph
    def identity(value: SERIES) -> SERIES:
        return value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return identity(value)

    assert bg.test_graph(graph, [4, 8]) == [4, 8]


def test_time_series_type_variable_preserves_final_guarantee():
    SERIES = TypeVar("SERIES", bound=bg.TimeSeries)

    @bg.graph
    def identity(value: SERIES) -> SERIES:
        return value

    @bg.source_node
    def source() -> bg.TS[int, bg.final]:
        return 11

    @bg.graph
    def graph() -> bg.TS[int, bg.final]:
        return identity(source())

    assert bg.test_graph(graph) == [11]


def test_distinct_typevars_with_the_same_name_do_not_share_a_mapping():
    LEFT = TypeVar("VALUE")
    RIGHT = TypeVar("VALUE")

    @bg.graph
    def select_left(left: bg.TS[LEFT], right: bg.TS[RIGHT]) -> bg.TS[LEFT]:
        return left

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[float]) -> bg.TS[int]:
        return select_left(left, right)

    assert bg.test_graph(graph, [9, 12], [0.5, 1.5]) == [9, 12]


def test_constrained_scalar_type_variable():
    NUMBER = TypeVar("NUMBER", int, float)

    @bg.graph
    def identity(value: bg.TS[NUMBER]) -> bg.TS[NUMBER]:
        return value

    @bg.graph
    def graph(value: bg.TS[float]) -> bg.TS[float]:
        return identity(value)

    assert bg.test_graph(graph, [1.5, 2.25]) == [1.5, 2.25]


def test_scalar_type_variable_preserves_final_guarantee_inside_time_series():
    VALUE = TypeVar("VALUE")

    @bg.graph
    def identity(value: bg.TS[VALUE, bg.final]) -> bg.TS[VALUE, bg.final]:
        return value

    @bg.source_node
    def source() -> bg.TS[int, bg.final]:
        return 14

    @bg.graph
    def graph() -> bg.TS[int, bg.final]:
        return identity(source())

    assert bg.test_graph(graph) == [14]


def test_scalar_type_variable_inside_time_series_set():
    VALUE = TypeVar("VALUE")

    @bg.compute_node
    def count(values: bg.TSS[VALUE]) -> bg.TS[int]:
        return len(values.value)

    @bg.source_node
    def source() -> bg.TSS[int]:
        return {2, 5, 8}

    @bg.graph
    def graph() -> bg.TS[int]:
        return count(source())

    assert bg.test_graph(graph) == [3]


def test_scalar_type_variable_inside_tuple_list_scalar_resolves_in_time_series_output():
    VALUE = TypeVar("VALUE")

    @bg.compute_node
    def as_list(value: bg.TS[VALUE]) -> bg.TS[tuple[VALUE, ...]]:
        return [value.value]

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[tuple[int, ...]]:
        return as_list(value)

    assert bg.test_graph(graph, [7]) == [(7,)]


def test_optional_scalar_cannot_be_a_homogeneous_tuple_item_type():
    with pytest.raises(TypeError, match=r"Optional\[\.\.\.\] cannot be used as a list item type"):

        @bg.source_node
        def invalid() -> bg.TS[tuple[Optional[int], ...]]:
            return [None]


def test_whole_time_series_type_variable_inside_dictionary():
    ITEM = TypeVar("ITEM", bound=bg.TimeSeries)

    @bg.graph
    def identity(values: bg.TSD[int, ITEM]) -> bg.TSD[int, ITEM]:
        return values

    @bg.source_node
    def source() -> bg.TSD[int, bg.TS[int]]:
        return {1: 17}

    @bg.graph
    def graph() -> bg.TS[int]:
        return bg.operators.getitem(identity(source()), 1)

    assert bg.test_graph(graph) == [17]


def test_time_series_typevar_cannot_be_used_as_scalar_payload():
    SERIES = TypeVar("SERIES", bound=bg.TimeSeries)

    with pytest.raises(ValueError, match="TS.*scalar annotation"):

        @bg.graph
        def invalid(value: bg.TS[SERIES]) -> bg.TS[int]:
            return bg.operators.const_(value=1)


def test_time_series_marker_is_only_valid_as_a_typevar_bound():
    with pytest.raises(TypeError, match="only valid as a TypeVar bound"):

        @bg.graph
        def invalid(value: bg.TimeSeries) -> bg.TS[int]:
            return bg.operators.const_(value=1)


def test_type_variable_descriptors_are_released_at_shutdown():
    result = subprocess.run(
        [
            sys.executable,
            "-c",
            "from typing import TypeVar; import bg; VALUE = TypeVar('VALUE'); bg.TS[VALUE]",
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert "nanobind: leaked" not in result.stderr
