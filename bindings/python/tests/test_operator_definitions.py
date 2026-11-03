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
    def graph(value: hg.TS[VALUE]) -> hg.TS[str]:
        return classify(value)

    assert hg.run_graph(graph, [1, 2]) == ["integer", "integer"]
    assert hg.run_graph(graph, ["a", "b"]) == ["text", "text"]


def test_operator_type_parameter_can_be_specialized_explicitly():
    VALUE = TypeVar("VALUE")

    @hg.operator(type_params=(VALUE,))
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
    def graph(value: hg.TS[int]) -> hg.TS[str]:
        return classify[int](value)

    assert hg.run_graph(graph, [1, 2]) == ["integer", "integer"]


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
