import sys

import pytest

import bg
from bg._bg_test_extension import (
    native_callable_gil,
    native_gil,
    native_node_lifecycle,
    native_python_exception,
    native_start_only_source,
    native_stop_exception,
    reset_native_node_lifecycle,
)


def test_aggregate_cpp_node_acquires_gil_for_callbacks_and_lifecycle():
    reset_native_node_lifecycle()

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return native_gil(value)

    assert bg.test_graph(graph, [4, bg.INVALID]) == [5, None]
    assert native_node_lifecycle() == (1, 1, 1, 1)


def test_callable_cpp_node_acquires_gil_before_input_and_output_access():
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return native_callable_gil(value)

    assert bg.test_graph(graph, [4, 9]) == [5, 10]


def test_python_aware_runtime_source_can_define_start_without_evaluate():
    reset_native_node_lifecycle()

    @bg.graph
    def graph() -> bg.TS[int]:
        return native_start_only_source()

    assert bg.test_graph(graph) == [42]
    assert native_node_lifecycle()[0] == 1


def test_native_cpp_node_propagates_python_exception_from_callback():
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return native_python_exception(value)

    with pytest.raises(ValueError, match="native node Python failure"):
        bg.test_graph(graph, [1])


def test_native_cpp_stop_reports_python_exception_unraisable_and_finalizes():
    reset_native_node_lifecycle()
    unraisable = []
    previous_hook = sys.unraisablehook

    @bg.graph
    def graph(value: bg.TS[int]) -> None:
        native_stop_exception(value)

    sys.unraisablehook = unraisable.append
    try:
        assert bg.test_graph(graph, [1]) == [None]
    finally:
        sys.unraisablehook = previous_hook

    assert len(unraisable) == 1
    assert isinstance(unraisable[0].exc_value, ValueError)
    assert str(unraisable[0].exc_value) == "native stop Python failure"
    assert native_node_lifecycle() == (0, 0, 1, 1)
