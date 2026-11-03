import os
import subprocess
import sys
from typing import TypeVar

import pytest

import hgraph as hg


def test_operator_uses_engine_overload_resolution_for_python_nodes():
    VALUE = TypeVar("VALUE")

    @hg.operator
    def classify(value: hg.TS[VALUE]) -> hg.TS[str]: ...

    @classify.overload
    @hg.compute_node
    def integer(value: hg.TS[int]) -> hg.TS[str]:
        return "integer"

    @classify.overload
    @hg.compute_node
    def text(value: hg.TS[str]) -> hg.TS[str]:
        return "text"

    @hg.graph
    def graph_int(value: hg.TS[int]) -> hg.TS[str]:
        return classify(value)

    @hg.graph
    def graph_text(value: hg.TS[str]) -> hg.TS[str]:
        return classify(value)

    assert hg.run_graph(graph_int, [1, 2]) == ["integer", "integer"]
    assert hg.run_graph(graph_text, ["a", "b"]) == ["text", "text"]


def test_operator_type_parameter_can_be_specialized_explicitly():
    VALUE = TypeVar("VALUE")

    @hg.operator(type_params=(VALUE,))
    def classify(value: hg.TS[VALUE]) -> hg.TS[str]: ...

    @classify.overload
    @hg.compute_node
    def stringify(value: hg.TS[VALUE]) -> hg.TS[str]:
        return str(value.value)

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[str]:
        return classify[int](value)

    assert hg.run_graph(graph, [1, 2]) == ["1", "2"]


def test_operator_defined_inside_graph_lives_through_deferred_compilation():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        @hg.operator
        def increment(input: hg.TS[int]) -> hg.TS[int]: ...

        @increment.overload
        @hg.compute_node
        def add_one(input: hg.TS[int]) -> hg.TS[int]:
            return input.value + 1

        return increment(value)

    assert hg.run_graph(graph, [1, 2]) == [2, 3]


def test_operator_accepts_a_python_graph_overload():
    @hg.operator
    def increment(value: hg.TS[int]) -> hg.TS[int]: ...

    @increment.overload
    @hg.graph
    def increment_graph(value: hg.TS[int]) -> hg.TS[int]:
        return value + 2

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return increment(value)

    assert hg.run_graph(graph, [3, 8]) == [5, 10]


def test_operator_reports_ambiguous_overloads_using_engine_diagnostics():
    @hg.operator
    def ambiguous(value: hg.TS[int]) -> hg.TS[int]: ...

    @ambiguous.overload
    @hg.compute_node
    def first(value: hg.TS[int]) -> hg.TS[int]:
        return 1

    @ambiguous.overload
    @hg.compute_node
    def second(value: hg.TS[int]) -> hg.TS[int]:
        return 2

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return ambiguous(value)

    with pytest.raises(ValueError, match="ambiguous"):
        hg.run_graph(graph, [0])


def test_operator_rejects_undecorated_overloads():
    @hg.operator
    def operation(value: hg.TS[int]) -> hg.TS[int]: ...

    with pytest.raises(TypeError, match="decorated @graph or Python node"):
        operation.overload(lambda value: value)


@pytest.mark.parametrize("overload_kind", ["node", "graph"])
def test_python_operator_overload_does_not_leak_at_shutdown(overload_kind: str):
    definition = (
        "@operation.overload\n"
        "@hg.compute_node\n"
        "def implementation(value: hg.TS[int]) -> hg.TS[int]:\n"
        "    return value\n"
        if overload_kind == "node"
        else "@operation.overload\n"
        "@hg.graph\n"
        "def implementation(value: hg.TS[int]) -> hg.TS[int]:\n"
        "    return value\n"
    )
    script = (
        "import hgraph as hg\n"
        "import gc\n"
        "@hg.operator\n"
        "def operation(value: hg.TS[int]) -> hg.TS[int]: ...\n"
        f"{definition}"
        "del implementation, operation\n"
        "gc.collect()\n"
    )
    process = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True,
        env=os.environ.copy(),
        text=True,
        check=False,
    )

    assert process.returncode == 0, process.stderr
    assert "nanobind: leaked" not in process.stderr
