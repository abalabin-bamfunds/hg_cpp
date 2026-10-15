from datetime import timedelta
import gc
import sys

import bg
import pytest


def test_run_graph_wires_scalar_arguments_as_constants_and_reacquires_gil_for_callbacks():
    values = []

    @bg.node
    def double(value: int) -> int:
        values.append(value)
        return value * 2

    @bg.graph
    def graph(value: int = 21) -> int:
        return double(value)

    assert bg.run_graph(graph) is None
    assert values == [21]

    values.clear()
    bg.run_graph(graph, value=12)
    assert values == [12]


def test_run_graph_binds_named_scalar_arguments_to_graph_parameters():
    values = []

    @bg.node
    def collect(left: int, right: int) -> int:
        values.append(left + right)
        return left + right

    @bg.graph
    def graph(left: int, right: int) -> int:
        return collect(left, right)

    bg.run_graph(graph, right=8, left=5)
    assert values == [13]


def test_run_graph_propagates_python_callback_exception_identity():
    error = ValueError("node callback failed")

    @bg.node
    def fail(value: int) -> int:
        raise error

    @bg.graph
    def graph() -> int:
        return fail(1)

    with pytest.raises(ValueError, match="node callback failed") as raised:
        bg.run_graph(graph)
    assert raised.value is error


@pytest.mark.parametrize("with_recorder", [False, True])
def test_run_graph_propagates_python_graph_wiring_exception_identity(with_recorder):
    error = LookupError("graph wiring callback failed")

    @bg.graph
    def graph() -> bg.TS[int]:
        raise error

    recorder = bg.EvaluationProfiler().recorder() if with_recorder else None
    with pytest.raises(LookupError, match="graph wiring callback failed") as raised:
        bg.run_graph(graph, recorder=recorder)
    assert raised.value is error


def test_run_graph_records_profile_and_exposes_post_run_queries():
    @bg.compute_node
    def increment(value: bg.TS[int]) -> bg.TS[int]:
        return value.value + 1

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return increment(value)

    profiler = bg.EvaluationProfiler(
        node_period=timedelta(microseconds=1),
        retained_node_periods=4,
        outer_period=timedelta(microseconds=1),
        retained_outer_periods=4,
    )

    recorder = profiler.recorder()
    assert bg.test_graph(graph, [1], recorder=recorder) == [2]
    graph_id = profiler.root_graph_id()
    assert isinstance(graph_id, int)
    assert profiler.graph_ids() == [graph_id]

    graph_info = profiler.graph(graph_id)
    assert graph_info["graph_id"] == graph_id
    assert graph_info["active"] is False
    assert graph_info["evaluations"] == 1

    periods = profiler.node_period_starts()
    assert periods
    nodes = profiler.node_periods(graph_id, periods[-1])
    assert nodes
    assert sum(node["evaluations"] for node in nodes) >= 1

    outer = profiler.outer_totals()
    assert outer["cycles"] == 1
    assert profiler.outer_periods()
    assert "Outer graph: 1 cycle" in profiler.report()


def test_run_graph_accepts_profiler_recorder():
    @bg.node
    def source(value: int) -> int:
        return value

    @bg.graph
    def graph() -> int:
        return source(7)

    profiler = bg.EvaluationProfiler()
    bg.run_graph(graph, recorder=profiler.recorder())

    assert profiler.root_graph_id() is not None
    assert profiler.outer_totals()["cycles"] == 1


def test_recorder_keeps_temporary_profiler_alive_for_the_run():
    @bg.node
    def source(value: int) -> int:
        return value

    @bg.graph
    def graph() -> int:
        return source(9)

    profiler = bg.EvaluationProfiler()
    owner_references = sys.getrefcount(profiler)
    recorder = profiler.recorder()
    assert sys.getrefcount(profiler) == owner_references + 1

    del recorder
    gc.collect()
    assert sys.getrefcount(profiler) == owner_references

    bg.run_graph(graph, recorder=bg.EvaluationProfiler().recorder())


@pytest.mark.parametrize("with_recorder", [False, True])
def test_test_graph_propagates_python_callback_exception_identity(with_recorder):
    error = LookupError("compute callback failed")

    @bg.compute_node
    def fail(value: bg.TS[int]) -> bg.TS[int]:
        raise error

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return fail(value)

    recorder = bg.EvaluationProfiler().recorder() if with_recorder else None
    with pytest.raises(LookupError, match="compute callback failed") as raised:
        bg.test_graph(graph, [1], recorder=recorder)
    assert raised.value is error
