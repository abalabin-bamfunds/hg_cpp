from datetime import datetime, timedelta
import inspect

import pytest

import bg


def test_compute_node_injects_clock_between_inputs():
    observed = []

    @bg.compute_node
    def add(left: bg.TS[int], clock: bg.EvaluationClock, right: bg.TS[int]) -> bg.TS[int]:
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
    def increment(value: int, clock: bg.EvaluationClock) -> int:
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
    schedulers = []

    @bg.source_node
    def source(clock: bg.EvaluationClock, scheduler: bg.Scheduler) -> bg.TS[int]:
        clocks.append(clock)
        schedulers.append(scheduler)
        return 1

    @bg.graph
    def graph() -> bg.TS[int]:
        return source()

    assert bg.test_graph(graph) == [1]
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = clocks[0].evaluation_time
    with pytest.raises(RuntimeError, match="outside its callback"):
        schedulers[0].schedule_at(datetime.now())


def test_keyword_only_clock_preserves_callback_parameter_order():
    observed = []

    @bg.compute_node
    def add(left: bg.TS[int], *, clock: bg.EvaluationClock, right: bg.TS[int]) -> bg.TS[int]:
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
    def total(*values: bg.TSL[bg.TS[int], bg.SIZE], clock: bg.EvaluationClock) -> bg.TS[int]:
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

    @bg.source_node
    def source(clock: bg.EvaluationClock, scheduler: bg.Scheduler) -> bg.TS[int]:
        evaluation_times.append(clock.evaluation_time)
        if len(evaluation_times) == 1:
            handles.append(scheduler.schedule_at(clock.evaluation_time + timedelta(seconds=1)))
            handles.append(scheduler.schedule_at(clock.evaluation_time + timedelta(seconds=2)))
        else:
            scheduler.cancel(handles[1])
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


def test_node_injectables_are_rejected_on_graphs_and_variadic_parameters():
    with pytest.raises(TypeError, match="only annotate Python node parameters"):

        @bg.graph
        def invalid(clock: bg.EvaluationClock) -> bg.TS[int]:
            return bg.operators.const_(1)

    with pytest.raises(TypeError, match="cannot annotate variadic"):

        @bg.compute_node
        def invalid(*scheduler: bg.Scheduler) -> bg.TS[int]:
            return 1
