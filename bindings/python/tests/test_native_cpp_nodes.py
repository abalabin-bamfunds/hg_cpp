import sys

import pytest

import hg
from hg._hg_test_extension import (
    native_callable_gil,
    native_gil,
    native_node_lifecycle,
    native_python_exception,
    native_python_object_property,
    native_start_only_source,
    native_stop_exception,
    reset_native_node_lifecycle,
)


def test_aggregate_cpp_node_acquires_gil_for_callbacks_and_lifecycle():
    reset_native_node_lifecycle()

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return native_gil(value)

    assert hg.run_graph(graph, [4, hg.INVALID]) == [5, None]
    assert native_node_lifecycle() == (1, 1, 1, 1)


def test_callable_cpp_node_acquires_gil_before_input_and_output_access():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return native_callable_gil(value)

    assert hg.run_graph(graph, [4, 9]) == [5, 10]


def test_python_aware_runtime_source_can_define_start_without_evaluate():
    reset_native_node_lifecycle()

    @hg.graph
    def graph() -> hg.TS[int]:
        return native_start_only_source()

    assert hg.run_graph(graph) == [42]
    assert native_node_lifecycle()[0] == 1


def test_native_cpp_node_propagates_python_exception_from_callback():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return native_python_exception(value)

    with pytest.raises(ValueError, match="native node Python failure"):
        hg.run_graph(graph, [1])


def test_native_cpp_node_converts_python_object_using_resolved_covariant_type():
    class Base:
        pass

    class Derived(Base):
        @property
        def native_result(self) -> "Derived":
            return self

    value = Derived()

    @hg.graph
    def graph(value: hg.TS[Base]) -> hg.TS[Base]:
        return native_python_object_property(value)

    result = hg.run_graph(graph, [value])[0]
    assert type(result) is Derived
    assert result is value


def test_native_cpp_node_rejects_builtin_scalar_for_python_object_input():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return native_python_object_property(value)

    with pytest.raises(ValueError, match="Cannot resolve operator native_python_object_property"):
        hg.run_graph(graph, [1])


def test_native_cpp_stop_reports_python_exception_unraisable_and_finalizes():
    reset_native_node_lifecycle()
    unraisable = []
    previous_hook = sys.unraisablehook

    @hg.graph
    def graph(value: hg.TS[int]) -> None:
        native_stop_exception(value)

    sys.unraisablehook = unraisable.append
    try:
        assert hg.run_graph(graph, [1]) is None
    finally:
        sys.unraisablehook = previous_hook

    assert len(unraisable) == 1
    assert isinstance(unraisable[0].exc_value, ValueError)
    assert str(unraisable[0].exc_value) == "native stop Python failure"
    assert native_node_lifecycle() == (0, 0, 1, 1)
