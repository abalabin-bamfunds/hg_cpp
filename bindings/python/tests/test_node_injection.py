from datetime import datetime, timedelta
import inspect
import subprocess
import sys

import pytest

import bg


def _run_realtime_script(source: str) -> None:
    result = subprocess.run([sys.executable, "-c", source], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


def test_compute_node_injects_clock_between_inputs():
    observed = []

    @bg.compute_node
    def add(left: bg.TS[int], right: bg.TS[int], clock: bg.EvaluationClock = None) -> bg.TS[int]:
        observed.append((clock.evaluation_time, clock.now))
        return left.value + right.value

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return add(left, right)

    assert list(inspect.signature(add).parameters) == ["left", "right"]
    assert bg.test_graph(graph, [1, 2], [10, 20]) == [11, 22]
    assert len(observed) == 2
    assert all(isinstance(evaluation_time, datetime) for evaluation_time, _ in observed)
    assert all(evaluation_time == now for evaluation_time, now in observed)


def test_node_injects_clock_without_exposing_it_as_a_wiring_argument():
    observed = []

    @bg.node
    def increment(value: int, clock: bg.EvaluationClock = None) -> int:
        observed.append(clock.evaluation_time)
        return value + 1

    @bg.graph
    def graph() -> bg.TS[int]:
        return increment(41)

    assert list(inspect.signature(increment).parameters) == ["value"]
    assert bg.test_graph(graph) == [42]
    assert len(observed) == 1
    assert isinstance(observed[0], datetime)


def test_source_node_clock_is_callback_scoped():
    clocks = []

    @bg.source_node
    def source(clock: bg.EvaluationClock = None) -> bg.TS[int]:
        clocks.append(clock)
        return 1

    @bg.graph
    def graph() -> bg.TS[int]:
        return source()

    assert bg.test_graph(graph) == [1]
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = clocks[0].evaluation_time


def test_keyword_only_clock_preserves_callback_parameter_order():
    observed = []

    @bg.compute_node
    def add(
        left: bg.TS[int], *, clock: bg.EvaluationClock = None, right: bg.TS[int]
    ) -> bg.TS[int]:
        observed.append(clock.evaluation_time)
        return left.value + right.value

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return add(left, right=right)

    assert list(inspect.signature(add).parameters) == ["left", "right"]
    assert bg.test_graph(graph, [2], [3]) == [5]
    assert len(observed) == 1


def test_keyword_only_clock_and_variadic_inputs_keep_callback_parameter_order():
    observed = []

    @bg.compute_node
    def total(
        *values: bg.TSL[bg.TS[int], bg.SIZE], clock: bg.EvaluationClock = None
    ) -> bg.TS[int]:
        observed.append(clock.evaluation_time)
        return sum(values.value)

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return total(left, right)

    assert list(inspect.signature(total).parameters) == ["values"]
    assert bg.test_graph(graph, [1, 2], [10, 20]) == [11, 22]
    assert len(observed) == 2


def test_scheduler_injects_and_cancels_timed_node_evaluations():
    evaluation_times = []
    handles = []
    observed_values = []

    @bg.node
    def source(
        clock: bg.EvaluationClock = None,
        scheduler: bg.Scheduler = None,
        state: bg.State[list] = None,
    ) -> int:
        evaluation_times.append(clock.evaluation_time)
        if len(evaluation_times) == 1:
            state.append(scheduler.schedule_at(clock.evaluation_time + timedelta(seconds=1)))
            state.append(scheduler.schedule_at(clock.evaluation_time + timedelta(seconds=2)))
            handles.extend(state)
        else:
            scheduler.cancel(state[1])
        return len(evaluation_times)

    @bg.compute_node
    def observe(value: bg.TS[int]) -> None:
        observed_values.append(value.value)

    @bg.graph
    def graph() -> None:
        observe(source())

    bg.run_graph(graph)

    assert len(handles) == 2
    assert all(handles)
    assert observed_values == [1, 2]
    assert evaluation_times[1] - evaluation_times[0] == timedelta(seconds=1)


def test_scheduler_handles_wall_clock_timers_and_preserves_callback_scope():
    _run_realtime_script(
        """
from datetime import timedelta
import bg

stop_source = bg.StopSource()
handles = []
fired = []
retained_schedulers = []
retained_times = []

@bg.node
def source(clock: bg.EvaluationClock = None, scheduler: bg.Scheduler = None) -> int:
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

@bg.graph
def graph() -> int:
    return source()

bg.run_graph(graph, engine=bg.EvaluationEngine.realtime, stop_token=stop_source.get_token())
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
    )


def test_node_injectables_are_rejected_on_graphs_and_variadic_parameters():
    with pytest.raises(TypeError, match="only annotate Python node parameters"):

        @bg.graph
        def invalid(clock: bg.EvaluationClock = None) -> bg.TS[int]:
            return bg.operators.const_(1)

    with pytest.raises(TypeError, match="cannot annotate variadic"):

        @bg.compute_node
        def invalid(*scheduler: bg.Scheduler) -> bg.TS[int]:
            return 1


def test_state_is_reused_per_node_and_isolated_between_nodes_and_runs():
    @bg.compute_node
    def count(value: bg.TS[int], state: bg.State[list] = None) -> bg.TS[int]:
        state.append(value.value)
        return len(state)

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return count(left) + count(right)

    assert list(inspect.signature(count).parameters) == ["value"]
    assert bg.test_graph(graph, [1, 2], [10, 20]) == [2, 4]
    assert bg.test_graph(graph, [3, 4], [30, 40]) == [2, 4]


def test_state_node_instances_with_identical_wiring_are_not_deduplicated():
    instance_ids = iter((1, 2))

    class StateValue:
        def __init__(self):
            self.instance_id = next(instance_ids)

    @bg.node
    def identify(value: int, state: bg.State[StateValue] = None) -> int:
        return state.instance_id

    @bg.graph
    def graph() -> bg.TS[int]:
        return identify(1) + identify(1)

    assert bg.test_graph(graph) == [3]


def test_state_factory_supports_non_weak_referenceable_callables():
    class StateFactory:
        __slots__ = ()

        def __call__(self):
            return []

    factory = StateFactory()

    @bg.compute_node
    def accumulate(value: bg.TS[int], state: bg.State[factory] = None) -> bg.TS[int]:
        state.append(value.value)
        return len(state)

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return accumulate(value)

    assert bg.test_graph(graph, [1, 2]) == [1, 2]


def test_state_is_isolated_between_nested_map_branches():
    @bg.compute_node
    def make_values(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {1: trigger.value, 2: trigger.value * 10}

    @bg.compute_node
    def count(value: bg.TS[int], state: bg.State[list] = None) -> bg.TS[int]:
        state.append(value.value)
        return len(state)

    @bg.graph
    def branch(value: bg.TS[int]) -> bg.TS[int]:
        return count(value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        mapped = bg.map(branch, make_values(trigger))
        return mapped[1] + mapped[2]

    assert bg.test_graph(graph, [1, 2]) == [2, 4]


def test_state_factory_exception_leaves_state_uninitialized():
    calls = []

    class BrokenState:
        def __init__(self):
            calls.append(None)
            raise ValueError("state construction failed")

    @bg.compute_node
    def copy(value: bg.TS[int], state: bg.State[BrokenState] = None) -> bg.TS[int]:
        return value.value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return copy(value)

    with pytest.raises(ValueError, match="state construction failed"):
        bg.test_graph(graph, [1])
    assert len(calls) == 1


def test_state_is_released_when_runtime_graph_is_destroyed():
    released = []

    class StateValue:
        def __del__(self):
            released.append(True)

    @bg.compute_node
    def copy(value: bg.TS[int], state: bg.State[StateValue] = None) -> bg.TS[int]:
        return value.value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return copy(value)

    assert bg.test_graph(graph, [1]) == [1]
    assert released == [True]


def test_state_requires_a_none_default_and_allows_only_one_parameter():
    with pytest.raises(TypeError, match="must use '= None'"):

        @bg.compute_node
        def missing_default(value: bg.TS[int], state: bg.State[list]) -> bg.TS[int]:
            return value.value

    with pytest.raises(TypeError, match="only one State parameter"):

        @bg.compute_node
        def duplicate(
            value: bg.TS[int], first: bg.State[list] = None, second: bg.State[dict] = None
        ) -> bg.TS[int]:
            return value.value


def test_clock_and_scheduler_require_none_defaults():
    with pytest.raises(TypeError, match="must use '= None'"):

        @bg.compute_node
        def missing_clock_default(
            value: bg.TS[int], clock: bg.EvaluationClock
        ) -> bg.TS[int]:
            return value.value

    with pytest.raises(TypeError, match="must use '= None'"):

        @bg.compute_node
        def missing_scheduler_default(scheduler: bg.Scheduler) -> bg.TS[int]:
            return 1
