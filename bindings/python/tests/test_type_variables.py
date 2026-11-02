import subprocess
import sys
from typing import Optional, TypeVar

import pytest

import hgraph as hg


def test_scalar_type_variable_in_time_series_input_and_output():
    VALUE = TypeVar("VALUE")

    @hg.graph
    def identity(value: hg.TS[VALUE]) -> hg.TS[VALUE]:
        return value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return identity(value)

    assert hg.run_graph(graph, [2, 5]) == [2, 5]


def test_scalar_type_variable_in_plain_graph_input_and_output():
    VALUE = TypeVar("VALUE")

    @hg.graph
    def identity(value: VALUE) -> VALUE:
        return value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return identity(value)

    assert hg.run_graph(graph, [3, 7]) == [3, 7]


def test_scalar_type_variable_in_compute_node_input_and_output():
    VALUE = TypeVar("VALUE")

    @hg.compute_node
    def increment(value: hg.TS[VALUE]) -> hg.TS[VALUE]:
        return value.value + 1

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return increment(value)

    assert hg.run_graph(graph, [1, 4]) == [2, 5]


def test_time_series_type_variable_forwarding():
    SERIES = TypeVar("SERIES", bound=hg.TimeSeries)

    @hg.graph
    def identity(value: SERIES) -> SERIES:
        return value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return identity(value)

    assert hg.run_graph(graph, [4, 8]) == [4, 8]


def test_time_series_type_variable_preserves_final_guarantee():
    SERIES = TypeVar("SERIES", bound=hg.TimeSeries)

    @hg.graph
    def identity(value: SERIES) -> SERIES:
        return value

    @hg.source_node
    def source() -> hg.TS[int, hg.final]:
        return 11

    @hg.graph
    def graph() -> hg.TS[int, hg.final]:
        return identity(source())

    assert hg.run_graph(graph) == [11]


def test_distinct_typevars_with_the_same_name_do_not_share_a_mapping():
    LEFT = TypeVar("VALUE")
    RIGHT = TypeVar("VALUE")

    @hg.graph
    def select_left(left: hg.TS[LEFT], right: hg.TS[RIGHT]) -> hg.TS[LEFT]:
        return left

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[float]) -> hg.TS[int]:
        return select_left(left, right)

    assert hg.run_graph(graph, [9, 12], [0.5, 1.5]) == [9, 12]


def test_constrained_scalar_type_variable():
    NUMBER = TypeVar("NUMBER", int, float)

    @hg.graph
    def identity(value: hg.TS[NUMBER]) -> hg.TS[NUMBER]:
        return value

    @hg.graph
    def graph(value: hg.TS[float]) -> hg.TS[float]:
        return identity(value)

    assert hg.run_graph(graph, [1.5, 2.25]) == [1.5, 2.25]


def test_scalar_type_variable_preserves_final_guarantee_inside_time_series():
    VALUE = TypeVar("VALUE")

    @hg.graph
    def identity(value: hg.TS[VALUE, hg.final]) -> hg.TS[VALUE, hg.final]:
        return value

    @hg.source_node
    def source() -> hg.TS[int, hg.final]:
        return 14

    @hg.graph
    def graph() -> hg.TS[int, hg.final]:
        return identity(source())

    assert hg.run_graph(graph) == [14]


def test_scalar_type_variable_inside_time_series_set():
    VALUE = TypeVar("VALUE")

    @hg.compute_node
    def count(values: hg.TSS[VALUE]) -> hg.TS[int]:
        return len(values.value)

    @hg.source_node
    def source() -> hg.TSS[int]:
        return {2, 5, 8}

    @hg.graph
    def graph() -> hg.TS[int]:
        return count(source())

    assert hg.run_graph(graph) == [3]


def test_scalar_type_variable_inside_tuple_list_scalar_resolves_in_time_series_output():
    VALUE = TypeVar("VALUE")

    @hg.compute_node
    def as_list(value: hg.TS[VALUE]) -> hg.TS[tuple[VALUE, ...]]:
        return [value.value]

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[tuple[int, ...]]:
        return as_list(value)

    assert hg.run_graph(graph, [7]) == [(7,)]


def test_optional_scalar_cannot_be_a_homogeneous_tuple_item_type():
    with pytest.raises(TypeError, match=r"Optional\[\.\.\.\] cannot be used as a list item type"):

        @hg.source_node
        def invalid() -> hg.TS[tuple[Optional[int], ...]]:
            return [None]


def test_whole_time_series_type_variable_inside_dictionary():
    ITEM = TypeVar("ITEM", bound=hg.TimeSeries)

    @hg.graph
    def identity(values: hg.TSD[int, ITEM]) -> hg.TSD[int, ITEM]:
        return values

    @hg.source_node
    def source() -> hg.TSD[int, hg.TS[int]]:
        return {1: 17}

    @hg.graph
    def graph() -> hg.TS[int]:
        return hg.operators.getitem(identity(source()), 1)

    assert hg.run_graph(graph) == [17]


def test_time_series_typevar_cannot_be_used_as_scalar_payload():
    SERIES = TypeVar("SERIES", bound=hg.TimeSeries)

    with pytest.raises(ValueError, match="TS.*scalar annotation"):

        @hg.graph
        def invalid(value: hg.TS[SERIES]) -> hg.TS[int]:
            return hg.operators.const_(value=1)


def test_time_series_marker_is_only_valid_as_a_typevar_bound():
    with pytest.raises(TypeError, match="only valid as a TypeVar bound"):

        @hg.graph
        def invalid(value: hg.TimeSeries) -> hg.TS[int]:
            return hg.operators.const_(value=1)


def test_type_variable_descriptors_are_released_at_shutdown():
    result = subprocess.run(
        [
            sys.executable,
            "-c",
            "from typing import TypeVar; import hgraph as hg; VALUE = TypeVar('VALUE'); hg.TS[VALUE]",
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert "nanobind: leaked" not in result.stderr
