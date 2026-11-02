import sys
from datetime import datetime, timedelta

import pytest

import hg


def test_node_lifecycle_callbacks_select_definition_arguments_in_declared_order():
    calls = []

    @hg.node
    def add(value: int, amount: int = 2) -> int:
        return value + amount

    @add.start
    def start(*, amount: int):
        calls.append(("start", amount))

    @add.stop
    def stop(value: int, amount: int):
        calls.append(("stop", value, amount))

    @hg.graph
    def graph() -> hg.TS[int]:
        return add(40, amount=2)

    assert hg.run_graph(graph) == [42]
    assert calls == [("start", 2), ("stop", 40, 2)]


def test_lifecycle_callbacks_work_for_a_sink_node():
    calls = []

    @hg.node
    def sink(value: int, scale: int) -> None:
        calls.append(("evaluate", value * scale))

    @sink.start
    def start(scale: int):
        calls.append(("start", scale))

    @sink.stop
    def stop(value: int):
        calls.append(("stop", value))

    @hg.graph
    def graph() -> None:
        sink(6, 7)

    hg.run_graph(graph)
    assert calls == [("start", 7), ("evaluate", 42), ("stop", 6)]


@pytest.mark.parametrize(
    "compute_node",
    [
        pytest.param(False, id="node"),
        pytest.param(True, id="compute_node"),
    ],
)
def test_node_start_return_is_ignored_before_later_evaluation(compute_node):
    starts = []

    if compute_node:
        node_decorator = hg.compute_node
        input_annotation = hg.TS[int]
        output_annotation = hg.TS[int]
        extract_value = lambda value: value.value
    else:
        node_decorator = hg.node
        input_annotation = int
        output_annotation = int
        extract_value = lambda value: value

    @hg.generator
    def source(clock: hg.EvaluationClock = None) -> hg.TS[int]:
        yield clock.evaluation_time + timedelta(microseconds=1), 5

    @node_decorator
    def identity(value: input_annotation) -> output_annotation:
        return extract_value(value)

    @identity.start
    def start() -> int:
        starts.append("start")
        return -1

    @hg.graph
    def graph() -> hg.TS[int]:
        return identity(source())

    actual = hg.run_graph(graph)
    assert starts == ["start"]
    assert actual == [None, 5]


def test_lifecycle_callback_registration_rejects_duplicates():
    @hg.compute_node
    def value(input: hg.TS[int]) -> hg.TS[int]:
        return input.value

    @value.start
    def start():
        pass

    with pytest.raises(TypeError, match="start callback is already defined"):

        @value.start
        def duplicate():
            pass


def test_invalid_lifecycle_callback_does_not_occupy_registration_slot():
    seen = []

    @hg.node
    def value(amount: int) -> int:
        return amount

    with pytest.raises(TypeError, match="is not a parameter"):

        @value.start
        def invalid(missing: int):
            pass

    @value.start
    def start(amount: int) -> int:
        seen.append(amount)
        return amount + 1

    @hg.graph
    def graph() -> hg.TS[int]:
        return value(3)

    assert hg.run_graph(graph) == [3]
    assert seen == [3]


def test_source_node_evaluates_at_start_and_can_schedule_itself():
    events = []

    @hg.source_node
    def source(
        state: hg.State[list] = None,
        clock: hg.EvaluationClock = None,
        scheduler: hg.Scheduler = None,
    ) -> hg.TS[int]:
        state.append(clock.evaluation_time)
        events.append(("evaluate", len(state)))
        if len(state) < 2:
            scheduler.schedule_at(clock.evaluation_time + timedelta(seconds=1))
        return len(state)

    @source.start
    def start():
        events.append(("start",))
        return object()

    @hg.compute_node
    def collect(value: hg.TS[int]) -> None:
        events.append(("output", value.value))

    @hg.graph
    def graph() -> None:
        collect(source())

    hg.run_graph(graph)
    assert events == [
        ("start",),
        ("evaluate", 1),
        ("output", 1),
        ("evaluate", 2),
        ("output", 2),
    ]


def test_source_node_passes_keyword_only_inputs_to_its_evaluation_function():
    @hg.source_node
    def source(*, value: int) -> hg.TS[int]:
        return value

    @hg.graph
    def graph() -> hg.TS[int]:
        return source(value=7)

    assert hg.run_graph(graph) == [7]


def test_sources_reject_invalidation_callbacks_without_time_series_inputs():
    @hg.source_node
    def source() -> hg.TS[int]:
        return 1

    with pytest.raises(TypeError, match="without time-series inputs"):

        @source.on_invalidation
        def invalidation():
            pass

    @hg.generator
    def generated() -> hg.TS[int]:
        yield timedelta(), 1

    with pytest.raises(TypeError, match="cannot register a separate start callback"):

        @generated.start
        def start():
            pass

    with pytest.raises(TypeError, match="without time-series inputs"):

        @generated.on_invalidation
        def invalidation():
            pass


def test_generator_reads_evaluation_clock_after_each_resume():
    observed = []

    @hg.generator
    def source(clock: hg.EvaluationClock = None) -> hg.TS[int]:
        start = clock.evaluation_time
        yield start, 0
        observed.append(clock.evaluation_time)
        yield start + timedelta(microseconds=1), 1
        observed.append(clock.evaluation_time)
        yield start + timedelta(microseconds=2), 2

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    actual = hg.run_graph(graph)

    assert actual == [0, 1, 2]
    assert len(observed) == 2
    assert observed[1] - observed[0] == timedelta(microseconds=1)


def test_empty_generator_is_a_valid_source():
    @hg.generator
    def source() -> hg.TS[int]:
        if False:
            yield datetime(2000, 1, 1), 1

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    assert hg.run_graph(graph) == [None]


def test_generator_requires_strictly_increasing_output_times():
    @hg.generator
    def source(clock: hg.EvaluationClock = None) -> hg.TS[int]:
        yield clock.evaluation_time, 1
        yield clock.evaluation_time, 2

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    with pytest.raises(ValueError, match="strictly increasing"):
        hg.run_graph(graph)


def test_generator_accepts_timedelta_relative_to_each_resume():
    @hg.generator
    def source() -> hg.TS[int]:
        yield timedelta(), 1
        yield timedelta(microseconds=1), 2

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    assert hg.run_graph(graph) == [1, 2]


def test_generator_retains_scalar_dictionary_arguments_across_yields():
    @hg.generator
    def source(data: dict[str, int], clock: hg.EvaluationClock = None) -> hg.TS[int]:
        start = clock.evaluation_time
        yield start + timedelta(microseconds=1), 1
        yield start + timedelta(microseconds=2), data["value"]

    @hg.graph
    def graph() -> hg.TS[int]:
        return source({"value": 42})

    assert hg.run_graph(graph) == [None, 1, 42]


def test_identical_generator_calls_create_independent_sources():
    values = []

    @hg.generator
    def source() -> hg.TS[int]:
        value = len(values) + 1
        values.append(value)
        yield timedelta(), value

    @hg.compute_node
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left.value + right.value

    @hg.graph
    def graph() -> hg.TS[int]:
        return add(source(), source())

    assert hg.run_graph(graph) == [3]
    assert values == [1, 2]


def test_generator_cleanup_reports_stop_failure_after_closing_pending_generator():
    events = []
    retained_clocks = []

    @hg.generator
    def source(clock: hg.EvaluationClock = None, name: str = "source") -> hg.TS[int]:
        try:
            yield clock.evaluation_time, 1
            events.append(("pending", name))
            yield clock.evaluation_time + timedelta(days=100), 2
        finally:
            generator_time = clock.evaluation_time
            try:
                retained_clocks[0].evaluation_time
            except RuntimeError as error:
                stop_clock_error = str(error)
            else:
                stop_clock_error = None
            events.append(("close", name, generator_time, stop_clock_error))

    @source.stop
    def stop(name: str, clock: hg.EvaluationClock = None):
        retained_clocks.append(clock)
        events.append(("stop", name, clock.evaluation_time))
        raise RuntimeError("stop failed")

    @hg.compute_node
    def fail(value: hg.TS[int]) -> None:
        raise ValueError("evaluation failed")

    @hg.graph
    def graph() -> None:
        fail(source(name="test"))

    previous_hook = sys.unraisablehook
    sys.unraisablehook = lambda failure: events.append(("unraisable", str(failure.exc_value)))
    try:
        with pytest.raises(ValueError, match="evaluation failed"):
            hg.run_graph(graph)
    finally:
        sys.unraisablehook = previous_hook

    assert [event[:2] for event in events] == [
        ("pending", "test"),
        ("stop", "test"),
        ("close", "test"),
        ("unraisable", "stop failed"),
    ]
    assert events[1][2] == events[2][2]
    assert "outside its callback" in events[2][3]
    with pytest.raises(RuntimeError, match="outside its callback"):
        retained_clocks[0].evaluation_time


def test_invalidation_callback_receives_selected_input():
    seen = []

    @hg.compute_node
    def identity(value: hg.TS[int]) -> hg.TS[int]:
        return value.value

    @identity.on_invalidation
    def invalidated(value: hg.TS[int]) -> int:
        seen.append(value.valid)
        return 99

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return identity(value)

    assert hg.run_graph(graph, [1, hg.INVALID]) == [1, 99]
    assert seen == [False]


@pytest.mark.parametrize("runner", [hg.run_graph, hg.run_graph])
def test_stop_callback_errors_are_reported_as_unraisable_after_stop(runner):
    calls = []

    @hg.node
    def value() -> int:
        return 1

    @value.stop
    def stop():
        calls.append("stop")
        raise RuntimeError("stop failed")

    @hg.graph
    def graph() -> hg.TS[int]:
        return value()

    previous_hook = sys.unraisablehook
    failures = []
    sys.unraisablehook = lambda failure: failures.append(
        (failure.exc_type, str(failure.exc_value), failure.object)
    )
    try:
        runner(graph)
    finally:
        sys.unraisablehook = previous_hook

    assert calls == ["stop"]
    assert len(failures) == 1
    assert failures[0] == (RuntimeError, "stop failed", stop)
