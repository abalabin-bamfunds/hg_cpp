from datetime import datetime, timedelta, timezone
import gc
import subprocess
import sys

import bg
import pytest


def _run_realtime_script(source: str) -> None:
    result = subprocess.run([sys.executable, "-c", source], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


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


def test_run_graph_defaults_to_simulation_and_accepts_a_start_time():
    observed = []

    @bg.node
    def source(clock: bg.EvaluationClock = None) -> int:
        observed.append(clock.evaluation_time)
        return 1

    @bg.graph
    def graph() -> int:
        return source()

    bg.run_graph(graph)
    start_time = datetime(2024, 5, 6, 7, 8, 9, 123456)
    bg.run_graph(graph, engine=bg.EvaluationEngine.simulation, start_time=start_time)

    assert len(observed) == 2
    assert observed[0] == datetime.fromtimestamp(0) + timedelta(microseconds=1)
    assert observed[1] == start_time


def test_run_graph_realtime_accepts_start_time_and_profile_recorder():
    _run_realtime_script(
        """
import datetime
import bg

default_observed = []
default_stop = bg.StopSource()
before = datetime.datetime.now()

@bg.node
def default_source(clock: bg.EvaluationClock = None) -> int:
    default_observed.append(clock.evaluation_time)
    default_stop.request_stop()
    return 3

@bg.graph
def default_graph() -> int:
    return default_source()

bg.run_graph(default_graph, engine=bg.EvaluationEngine.realtime, stop_token=default_stop.get_token())
after = datetime.datetime.now()
assert len(default_observed) == 1
assert before <= default_observed[0] <= after

observed = []
stop_source = bg.StopSource()
start_time = datetime.datetime(2024, 5, 6, 7, 8, 9, 123456)

@bg.node
def source(clock: bg.EvaluationClock = None) -> int:
    observed.append(clock.evaluation_time)
    stop_source.request_stop()
    return 7

@bg.graph
def graph() -> int:
    return source()

profiler = bg.EvaluationProfiler()
bg.run_graph(
    graph,
    engine=bg.EvaluationEngine.realtime,
    start_time=start_time,
    stop_token=stop_source.get_token(),
    recorder=profiler.recorder(),
)
assert observed == [start_time]
assert profiler.outer_totals()["cycles"] == 1
assert profiler.root_graph_id() is not None
"""
    )


def test_run_graph_realtime_stop_from_another_thread_completes_downstream_tick():
    _run_realtime_script(
        """
import threading
import bg

entered = threading.Event()
received = []
errors = []
stop_source = bg.StopSource()

@bg.node
def source() -> int:
    entered.set()
    return 41

@bg.compute_node
def sink(value: bg.TS[int]) -> None:
    received.append(value.value)

@bg.graph
def graph() -> None:
    sink(source())

def stop_after_callback():
    if not entered.wait(3):
        errors.append(RuntimeError("the source callback did not start"))
        return
    stop_source.request_stop()

thread = threading.Thread(target=stop_after_callback)
thread.start()
bg.run_graph(graph, engine=bg.EvaluationEngine.realtime, stop_token=stop_source.get_token())
thread.join(3)
assert not thread.is_alive()
assert not errors, repr(errors)
assert received == [41]
"""
    )


def test_run_graph_realtime_honors_pre_requested_token_and_token_lifetime():
    _run_realtime_script(
        """
import bg

stop_source = bg.StopSource()
token = stop_source.get_token()
empty_token = bg.StopToken()
assert not empty_token.stop_requested()
assert isinstance(token, bg.StopToken)
assert not token.stop_requested()
assert not stop_source.stop_requested()
assert stop_source.request_stop()
assert stop_source.stop_requested()
assert not stop_source.request_stop()
assert token.stop_requested()
del stop_source
assert token.stop_requested()

calls = []
@bg.node
def source() -> int:
    calls.append(True)
    return 1

@bg.graph
def graph() -> int:
    return source()

bg.run_graph(graph, engine=bg.EvaluationEngine.realtime, stop_token=token)
assert calls == []
"""
    )


def test_runner_options_validate_engine_start_time_and_stop_token():
    @bg.graph
    def graph() -> int:
        return 1

    with pytest.raises(TypeError, match="engine must be a bg.EvaluationEngine"):
        bg.run_graph(graph, engine="realtime")
    with pytest.raises(TypeError, match="start_time must be a datetime or None"):
        bg.run_graph(graph, start_time=1)
    with pytest.raises(ValueError, match="timezone-naive"):
        bg.run_graph(graph, start_time=datetime.now(timezone.utc))
    with pytest.raises(TypeError, match="stop_token must be a bg.StopToken or None"):
        bg.run_graph(graph, stop_token=bg.StopSource())


def test_test_graph_uses_simulation_start_time_and_rejects_realtime_before_wiring():
    observed = []
    wired = []

    @bg.compute_node
    def observe(value: bg.TS[int], clock: bg.EvaluationClock = None) -> bg.TS[int]:
        observed.append(clock.evaluation_time)
        return value.value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        wired.append(True)
        return observe(value)

    start_time = datetime(2024, 5, 6, 7, 8, 9, 123456)
    assert bg.test_graph(graph, [1, 2], engine=bg.EvaluationEngine.simulation, start_time=start_time) == [1, 2]
    assert observed == [start_time, start_time + timedelta(microseconds=1)]
    wired.clear()

    with pytest.raises(ValueError, match="test_graph only supports the simulation engine"):
        bg.test_graph(graph, [3], engine=bg.EvaluationEngine.realtime)
    assert wired == []


def test_test_graph_rejects_invalid_start_times():
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return value

    with pytest.raises(TypeError, match="start_time must be a datetime or None"):
        bg.test_graph(graph, [1], start_time=1)
    with pytest.raises(ValueError, match="timezone-naive"):
        bg.test_graph(graph, [1], start_time=datetime.now(timezone.utc))
