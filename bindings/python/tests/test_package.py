import gc
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import weakref

import bg
import pytest
from bg import _bg


def test_imports_the_expected_package():
    expected_package = os.environ.get("BG_PYTHON_PACKAGE_DIR")
    if expected_package is None:
        pytest.skip("BG_PYTHON_PACKAGE_DIR is not set")

    package_dir = Path(expected_package).resolve()
    assert Path(bg.__file__).resolve().parent == package_dir

    extension = importlib.util.find_spec("bg._bg")
    assert extension is not None
    assert Path(extension.origin).resolve().parent == package_dir


def test_extension_modules_share_python_conversion_registry():
    extension = pytest.importorskip("bg._bg_test_extension")

    assert _bg._scalar_conversions_address() == extension.scalar_conversions_address()


def test_symbol_is_a_value_type():
    value = bg.symbol("price")

    assert value == bg.Symbol("price")
    assert hash(value) == hash(bg.symbol("price"))
    assert str(value) == "price"
    assert repr(value) == "symbol('price')"


def test_null_symbol_is_distinct_from_empty_symbol():
    null_symbol = bg.Symbol()

    assert null_symbol != bg.symbol("")
    assert repr(null_symbol) == "symbol()"


def test_graph_wires_python_nodes_and_runs_each_input_tick():
    @bg.compute_node
    def increment(value: bg.TS[int]) -> bg.TS[int]:
        return value.value + 1

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return increment(value)

    assert bg.test_graph(graph, [1, 2, None, 4]) == [2, 3, None, 5]


def test_node_decorator_preserves_function_metadata():
    @bg.compute_node
    def identity(value: bg.TS[int]) -> bg.TS[int]:
        """Return the supplied value."""
        return value.value

    assert identity.__name__ == "identity"
    assert identity.__doc__ == "Return the supplied value."
    assert identity.__wrapped__ is identity.function


def test_graph_composes_nested_graphs_and_scalar_arguments():
    @bg.compute_node
    def add(value: bg.TS[int], offset: int) -> bg.TS[int]:
        return value.value + offset

    @bg.graph
    def add_default(value: bg.TS[int], offset: int = 3) -> bg.TS[int]:
        return add(value, offset=offset)

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return add_default(value)

    assert bg.test_graph(graph, [1, 2]) == [4, 5]


def test_graph_wiring_retains_a_locally_declared_node_until_execution():
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        @bg.compute_node
        def increment(inner: bg.TS[int]) -> bg.TS[int]:
            return inner.value + 1

        return increment(value)

    assert bg.test_graph(graph, [2, 4]) == [3, 5]


def test_python_source_node_runs_at_graph_start():
    @bg.source_node
    def source() -> bg.TS[int]:
        return 7

    @bg.graph
    def graph() -> bg.TS[int]:
        return source()

    assert bg.test_graph(graph) == [7]


def test_python_time_series_input_accepts_a_constant():
    @bg.compute_node
    def identity(value: bg.TS[int]) -> bg.TS[int]:
        return value.value

    @bg.graph
    def graph() -> bg.TS[int]:
        return identity(7)

    assert bg.test_graph(graph) == [7]


def test_python_node_receives_optional_scalar_as_none():
    @bg.compute_node
    def add_optional(value: bg.TS[int], offset: int | None = None) -> bg.TS[int]:
        return value.value + (offset or 0)

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return add_optional(value, offset=None)

    assert bg.test_graph(graph, [7]) == [7]


@pytest.mark.parametrize(
    ("annotation", "value"),
    [
        (bool, True),
        (float, 2.5),
        (bg.Symbol, bg.symbol("market")),
    ],
)
def test_source_node_scalar_outputs(annotation, value):
    @bg.source_node
    def source() -> bg.TS[annotation]:
        return value

    @bg.graph
    def graph() -> bg.TS[annotation]:
        return source()

    assert bg.test_graph(graph) == [value]


def test_python_string_and_symbol_scalar_arguments():
    @bg.source_node
    def source() -> bg.TS[int]:
        return 1

    @bg.compute_node
    def add_text(value: bg.TS[int], text: str, label: bg.Symbol) -> bg.TS[int]:
        return value.value + len(text) + len(str(label))

    @bg.graph
    def graph() -> bg.TS[int]:
        return add_text(source(), text="market", label=bg.symbol("open"))

    assert bg.test_graph(graph) == [1 + len("market") + len("open")]


def test_input_view_expires_after_the_callback():
    saved_inputs = []

    @bg.compute_node
    def save_input(value: bg.TS[int]) -> bg.TS[int]:
        saved_inputs.append(value)
        return value.value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return save_input(value)

    assert bg.test_graph(graph, [7]) == [7]
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = saved_inputs[0].value


def test_input_view_from_an_earlier_callback_stays_expired():
    saved_inputs = []

    @bg.compute_node
    def save_input(value: bg.TS[int]) -> bg.TS[int]:
        if saved_inputs:
            with pytest.raises(RuntimeError, match="outside its callback"):
                _ = saved_inputs[-1].value
        saved_inputs.append(value)
        return value.value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return save_input(value)

    assert bg.test_graph(graph, [7, 8]) == [7, 8]


def test_wiring_point_expires_after_graph_construction():
    saved_points = []

    @bg.compute_node
    def identity(value: bg.TS[int]) -> bg.TS[int]:
        return value.value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        saved_points.append(value)
        return identity(value)

    assert bg.test_graph(graph, [7]) == [7]
    with pytest.raises(RuntimeError, match="outside its graph construction"):
        _ = saved_points[0].has_output


def test_repeated_node_wiring_is_deduplicated():
    evaluations = []

    @bg.compute_node
    def increment(value: bg.TS[int]) -> bg.TS[int]:
        evaluations.append(value.value)
        return value.value + 1

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        increment(value)
        return increment(value)

    assert bg.test_graph(graph, [3]) == [4]
    assert evaluations == [3]


def test_callback_exception_propagates_from_test_graph():
    @bg.compute_node
    def fail(value: bg.TS[int]) -> bg.TS[int]:
        raise ValueError("callback failed")

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return fail(value)

    with pytest.raises(ValueError, match="callback failed"):
        bg.test_graph(graph, [3])


def test_none_requests_no_output_change():
    @bg.compute_node
    def no_change(value: bg.TS[int]) -> bg.TS[int]:
        return None

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return no_change(value)

    assert bg.test_graph(graph, [3]) == [None]


def test_sink_graph_runs_without_an_output():
    received = []

    @bg.compute_node
    def sink(value: bg.TS[int]) -> None:
        received.append(value.value)

    @bg.graph
    def graph(value: bg.TS[int]) -> None:
        sink(value)

    assert bg.test_graph(graph, [3, 5]) == [None, None]
    assert received == [3, 5]


def test_python_callback_receives_interleaved_time_series_and_scalars():
    @bg.compute_node
    def combine(left: bg.TS[int], offset: int, right: bg.TS[float], scale: float) -> bg.TS[float]:
        return (left.value + offset) * scale + right.value

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[float]) -> bg.TS[float]:
        return combine(left, 2, right, 0.5)

    assert bg.test_graph(graph, [2, 4], [10.0, 20.0]) == [12.0, 23.0]


def test_module_scope_definitions_do_not_leak_native_instances_at_shutdown():
    script = """
import bg

@bg.compute_node
def identity(value: bg.TS[int]) -> bg.TS[int]:
    return value.value

@bg.graph
def graph(value: bg.TS[int]) -> bg.TS[int]:
    return identity(value)
"""

    completed = subprocess.run(
        [sys.executable, "-c", script],
        check=False,
        capture_output=True,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert "nanobind: leaked" not in completed.stderr


def test_graph_definition_retains_and_releases_python_callback():
    def make_graph():
        def graph() -> None:
            pass

        return graph, weakref.ref(graph)

    callback, callback_ref = make_graph()
    definition = bg.graph(callback)

    del callback
    gc.collect()
    assert callback_ref() is definition.function

    del definition
    gc.collect()
    assert callback_ref() is None


def test_node_definition_retains_and_releases_python_callback():
    def make_node():
        def identity(value: bg.TS[int]) -> bg.TS[int]:
            return value.value

        return identity, weakref.ref(identity)

    callback, callback_ref = make_node()
    definition = bg.compute_node(callback)

    del callback
    gc.collect()
    assert callback_ref() is definition.function

    del definition
    gc.collect()
    assert callback_ref() is None
