from datetime import datetime, timedelta
import inspect
import subprocess
import sys

import pytest

import hg


def test_compute_node_injects_clock_between_inputs():
    calls = 0

    @hg.compute_node
    def add(left: hg.TS[int], right: hg.TS[int], clock: hg.EvaluationClock = None) -> hg.TS[int]:
        nonlocal calls
        calls += 1
        assert isinstance(clock.evaluation_time, datetime)
        assert clock.evaluation_time == clock.now
        return left.value + right.value

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return add(left, right)

    assert list(inspect.signature(add).parameters) == ["left", "right"]
    assert hg.run_graph(graph, [1, 2], [10, 20]) == [11, 22]
    assert calls == 2


def test_node_injects_clock_without_exposing_it_as_a_wiring_argument():
    calls = 0

    @hg.node
    def increment(value: int, clock: hg.EvaluationClock = None) -> int:
        nonlocal calls
        calls += 1
        assert isinstance(clock.evaluation_time, datetime)
        return value + 1

    @hg.graph
    def graph() -> hg.TS[int]:
        return increment(41)

    assert list(inspect.signature(increment).parameters) == ["value"]
    assert hg.run_graph(graph) == [42]
    assert calls == 1


def test_source_node_clock_is_callback_scoped():
    clocks = []

    @hg.source_node
    def source(clock: hg.EvaluationClock = None) -> hg.TS[int]:
        clocks.append(clock)
        return 1

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    assert hg.run_graph(graph) == [1]
    assert len(clocks) == 1
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = clocks[0].evaluation_time


def test_keyword_only_clock_preserves_callback_parameter_order():
    calls = 0

    @hg.compute_node
    def add(
        left: hg.TS[int], *, clock: hg.EvaluationClock = None, right: hg.TS[int]
    ) -> hg.TS[int]:
        nonlocal calls
        calls += 1
        assert isinstance(clock.evaluation_time, datetime)
        return left.value + right.value

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return add(left, right=right)

    assert list(inspect.signature(add).parameters) == ["left", "right"]
    assert hg.run_graph(graph, [2], [3]) == [5]
    assert calls == 1


def test_keyword_only_clock_and_variadic_inputs_keep_callback_parameter_order():
    calls = 0

    @hg.compute_node
    def total(
        *values: hg.TSL[hg.TS[int], hg.SIZE], clock: hg.EvaluationClock = None
    ) -> hg.TS[int]:
        nonlocal calls
        calls += 1
        assert isinstance(clock.evaluation_time, datetime)
        return sum(values.value)

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return total(left, right)

    assert list(inspect.signature(total).parameters) == ["values"]
    assert hg.run_graph(graph, [1, 2], [10, 20]) == [11, 22]
    assert calls == 2


def test_scheduler_injects_and_cancels_timed_node_evaluations():
    evaluation_times = []
    handles = []
    observed_values = []

    @hg.node
    def source(
        clock: hg.EvaluationClock = None,
        scheduler: hg.Scheduler = None,
        state: hg.State[list] = None,
    ) -> int:
        evaluation_times.append(clock.evaluation_time)
        if len(evaluation_times) == 1:
            state.append(scheduler.schedule_at(clock.evaluation_time + timedelta(seconds=1)))
            state.append(scheduler.schedule_at(clock.evaluation_time + timedelta(seconds=2)))
            handles.extend(state)
        else:
            scheduler.cancel(state[1])
        return len(evaluation_times)

    @hg.compute_node
    def observe(value: hg.TS[int]) -> None:
        observed_values.append(value.value)

    @hg.graph
    def graph() -> None:
        observe(source())

    hg.run_graph(graph)

    assert len(handles) == 2
    assert all(handles)
    assert observed_values == [1, 2]
    assert evaluation_times[1] - evaluation_times[0] == timedelta(seconds=1)


def test_scheduler_handles_wall_clock_timers_and_preserves_callback_scope():
    script = """
from datetime import timedelta
import hg

stop_source = hg.StopSource()
handles = []
fired = []
retained_schedulers = []
retained_times = []

@hg.node
def source(clock: hg.EvaluationClock = None, scheduler: hg.Scheduler = None) -> int:
    retained_schedulers.append(scheduler)
    retained_times.append(clock.now)
    if not handles:
        deadline = clock.now - timedelta(seconds=1)
        handles.append(scheduler.schedule_wall_clock_at(deadline))
        handles.append(scheduler.schedule_wall_clock_at(deadline))
        assert not scheduler.timer_fired(handles[0])
        assert not scheduler.timer_fired(handles[1])
        scheduler.cancel(handles[1])
        return 1

    assert scheduler.timer_fired(handles[0])
    assert not scheduler.timer_fired(handles[1])
    fired.append(True)
    stop_source.request_stop()
    return 2

@hg.graph
def graph() -> int:
    return source()

hg.run_graph(graph, engine=hg.EvaluationEngine.realtime, stop_token=stop_source.get_token())
assert fired == [True]
try:
    retained_schedulers[0].timer_fired(handles[0])
except RuntimeError as error:
    assert "outside its callback" in str(error)
else:
    raise AssertionError("a retained Scheduler must reject access after its callback")
try:
    retained_schedulers[0].schedule_wall_clock_at(retained_times[0])
except RuntimeError as error:
    assert "outside its callback" in str(error)
else:
    raise AssertionError("a retained Scheduler must reject scheduling after its callback")
"""
    result = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


def test_node_injectables_are_rejected_on_graphs_and_variadic_parameters():
    with pytest.raises(TypeError, match="only annotate Python node parameters"):

        @hg.graph
        def invalid(clock: hg.EvaluationClock = None) -> hg.TS[int]:
            return hg.operators.const_(1)

    with pytest.raises(TypeError, match="cannot annotate variadic"):

        @hg.compute_node
        def invalid(*scheduler: hg.Scheduler) -> hg.TS[int]:
            return 1


def test_state_is_reused_per_node_and_isolated_between_nodes_and_runs():
    @hg.compute_node
    def count(value: hg.TS[int], state: hg.State[list] = None) -> hg.TS[int]:
        state.append(value.value)
        return len(state)

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return count(left) + count(right)

    assert list(inspect.signature(count).parameters) == ["value"]
    assert hg.run_graph(graph, [1, 2], [10, 20]) == [2, 4]
    assert hg.run_graph(graph, [3, 4], [30, 40]) == [2, 4]


def test_state_node_instances_with_identical_wiring_are_not_deduplicated():
    instance_ids = iter((1, 2))

    class StateValue:
        def __init__(self):
            self.instance_id = next(instance_ids)

    @hg.node
    def identify(value: int, state: hg.State[StateValue] = None) -> int:
        return state.instance_id

    @hg.graph
    def graph() -> hg.TS[int]:
        return identify(1) + identify(1)

    assert hg.run_graph(graph) == [3]


def test_state_factory_supports_non_weak_referenceable_callables():
    class StateFactory:
        __slots__ = ()

        def __call__(self):
            return []

    factory = StateFactory()

    @hg.compute_node
    def accumulate(value: hg.TS[int], state: hg.State[factory] = None) -> hg.TS[int]:
        state.append(value.value)
        return len(state)

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return accumulate(value)

    assert hg.run_graph(graph, [1, 2]) == [1, 2]


def test_state_is_isolated_between_nested_map_branches():
    @hg.compute_node
    def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: trigger.value, 2: trigger.value * 10}

    @hg.compute_node
    def count(value: hg.TS[int], state: hg.State[list] = None) -> hg.TS[int]:
        state.append(value.value)
        return len(state)

    @hg.graph
    def branch(value: hg.TS[int]) -> hg.TS[int]:
        return count(value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(branch, make_values(trigger))
        return mapped[1] + mapped[2]

    assert hg.run_graph(graph, [1, 2]) == [2, 4]


def test_state_factory_exception_leaves_state_uninitialized():
    calls = []

    class BrokenState:
        def __init__(self):
            calls.append(None)
            raise ValueError("state construction failed")

    @hg.compute_node
    def copy(value: hg.TS[int], state: hg.State[BrokenState] = None) -> hg.TS[int]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return copy(value)

    with pytest.raises(ValueError, match="state construction failed"):
        hg.run_graph(graph, [1])
    assert len(calls) == 1


def test_state_is_released_when_runtime_graph_is_destroyed():
    released = []

    class StateValue:
        def __del__(self):
            released.append(True)

    @hg.compute_node
    def copy(value: hg.TS[int], state: hg.State[StateValue] = None) -> hg.TS[int]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return copy(value)

    assert hg.run_graph(graph, [1]) == [1]
    assert released == [True]


def test_state_requires_a_none_default_and_allows_only_one_parameter():
    with pytest.raises(TypeError, match="must use '= None'"):

        @hg.compute_node
        def missing_default(value: hg.TS[int], state: hg.State[list]) -> hg.TS[int]:
            return value.value

    with pytest.raises(TypeError, match="only one State parameter"):

        @hg.compute_node
        def duplicate(
            value: hg.TS[int], first: hg.State[list] = None, second: hg.State[dict] = None
        ) -> hg.TS[int]:
            return value.value


def test_clock_and_scheduler_require_none_defaults():
    with pytest.raises(TypeError, match="must use '= None'"):

        @hg.compute_node
        def missing_clock_default(
            value: hg.TS[int], clock: hg.EvaluationClock
        ) -> hg.TS[int]:
            return value.value

    with pytest.raises(TypeError, match="must use '= None'"):

        @hg.compute_node
        def missing_scheduler_default(scheduler: hg.Scheduler) -> hg.TS[int]:
            return 1
