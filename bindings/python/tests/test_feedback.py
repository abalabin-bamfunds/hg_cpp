from datetime import timedelta
from typing import TypeVar

import pytest

import hg


VALUE = TypeVar("VALUE")


def test_feedback_export_alias_and_inferred_wiring():
    @hg.graph
    def inferred(value: hg.TS[int]) -> hg.TS[int]:
        return hg.feedback(value, "inferred")

    @hg.graph
    def operator_alias(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.feedback(value, "alias")

    assert hg.operators.feedback is hg.feedback
    assert hg.run_graph(inferred, [2, 5, 7]) == [None, 2, 5, 7]
    assert hg.run_graph(operator_alias, [3, 4]) == [None, 3, 4]


def test_feedback_object_binds_a_bounded_computation_loop():
    @hg.node
    def advance(seed: int | None, previous: int | None) -> int:
        if previous is not None:
            return previous + 1 if previous < 3 else None
        return seed

    @hg.graph
    def graph(seed: hg.TS[int]) -> hg.TS[int]:
        loop = hg.Feedback(hg.TS[int], name="bounded")
        next_value = advance(seed, loop.output)
        loop.bind(next_value)
        return loop.output

    assert hg.run_graph(graph, [1]) == [None, 1, 2, 3]


def test_feedback_replays_sparse_updates_and_invalidations():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return hg.feedback(value)

    assert hg.run_graph(graph, [None, 3, None, 8]) == [None, None, 3, None, 8]
    assert hg.run_graph(graph, [1, hg.INVALID, 3]) == [None, 1, None, 3]


def test_feedback_replays_at_the_next_minimum_time():
    source_times = []
    replay_times = []

    @hg.compute_node
    def record_source(value: hg.TS[int], clock: hg.EvaluationClock = None) -> None:
        source_times.append(clock.evaluation_time)

    @hg.compute_node
    def record_replay(value: hg.TS[int], clock: hg.EvaluationClock = None) -> None:
        replay_times.append(clock.evaluation_time)

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        delayed = hg.feedback(value)
        record_source(value)
        record_replay(delayed)
        return value

    hg.run_graph(graph, [1, 2])

    assert len(source_times) == 2
    assert replay_times == [time + timedelta(microseconds=1) for time in source_times]


def test_feedback_replays_dictionary_updates_and_removals():
    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 1:
            return {1: 10}
        if trigger.value == 2:
            return {1: 20}
        return {1: hg.REMOVE}

    @hg.compute_node
    def inspect(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return values.value.get(1, 0)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        loop = hg.Feedback(hg.TSD[int, hg.TS[int]])
        result = inspect(loop.output)
        loop.bind(make_dictionary(trigger))
        return result

    assert hg.run_graph(graph, [1, 2, 3]) == [None, 10, 20, 0]


@pytest.mark.parametrize("explicit_type", [False, True])
def test_feedback_samples_reference_sources_before_retaining_updates(explicit_type):
    @hg.compute_node
    def select(
        use_second: hg.TS[bool],
        first: hg.REF[hg.TSD[int, hg.TS[int]]],
        second: hg.REF[hg.TSD[int, hg.TS[int]]],
    ) -> hg.REF[hg.TSD[int, hg.TS[int]]]:
        return (second if use_second.value else first).value

    @hg.compute_node
    def total(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return sum(values.value.values())

    @hg.graph
    def graph(
        use_second: hg.TS[bool],
        first: hg.TSD[int, hg.TS[int]],
        second: hg.TSD[int, hg.TS[int]],
    ) -> hg.TS[int]:
        selected = select(use_second, first, second)
        if explicit_type:
            loop = hg.Feedback(hg.TSD[int, hg.TS[int]])
            result = total(loop.output)
            loop.bind(selected)
            return result
        return total(hg.feedback(selected))

    assert hg.run_graph(
        graph,
        [False, True],
        [{1: 10}, {1: 10}],
        [{2: 20}, {2: 20}],
    ) == [None, 10, 20]


def test_feedback_inference_changes_const_to_final():
    @hg.graph
    def graph() -> hg.TS[int, hg.final]:
        return hg.feedback(hg.operators.const_(7))

    assert hg.run_graph(graph) == [None, 7]


def test_feedback_final_output_accepts_a_const_source():
    @hg.graph
    def graph() -> hg.TS[int, hg.final]:
        loop = hg.Feedback(hg.TS[int, hg.final])
        loop.bind(hg.operators.const_(7))
        return loop.output

    assert hg.run_graph(graph) == [None, 7]


def test_feedback_rejects_invalid_output_types():
    with pytest.raises(ValueError, match="time-series type"):
        hg.Feedback(int)

    with pytest.raises(RuntimeError, match="references"):
        @hg.graph
        def reference_output() -> hg.TS[int]:
            return hg.Feedback(hg.REF[hg.TS[int]]).output

        hg.run_graph(reference_output)

    with pytest.raises(RuntimeError, match="const guarantee"):
        @hg.graph
        def const_output() -> hg.TS[int]:
            return hg.Feedback(hg.TS[int, hg.const_]).output

        hg.run_graph(const_output)

    with pytest.raises(RuntimeError, match="resolved"):
        @hg.graph
        def unresolved_output() -> hg.TS[int]:
            return hg.Feedback(hg.TS[VALUE]).output

        hg.run_graph(unresolved_output)


def test_feedback_rejects_incompatible_input_types():
    @hg.graph
    def graph(value: hg.TS[str]) -> hg.TS[int]:
        loop = hg.Feedback(hg.TS[int])
        loop.bind(value)
        return loop.output

    with pytest.raises(RuntimeError, match="feedback input"):
        hg.run_graph(graph, ["value"])


def test_feedback_rejects_use_from_another_graph():
    captured = []

    @hg.graph
    def branch(value: hg.TS[int]) -> hg.TS[int]:
        captured[0].bind(value)
        return value

    @hg.compute_node
    def make_values(value: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: value.value}

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        loop = hg.Feedback(hg.TS[int])
        captured[:] = [loop]
        mapped = hg.map(branch, make_values(value))
        return mapped[1]

    with pytest.raises(RuntimeError, match="outside its graph construction"):
        hg.run_graph(graph, [1])


def test_feedback_rejects_duplicate_binding():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        loop = hg.Feedback(hg.TS[int])
        loop.bind(value)
        loop.bind(value)
        return loop.output

    with pytest.raises(RuntimeError, match="assigned only once"):
        hg.run_graph(graph, [1])


def test_feedback_rejects_expired_handles():
    saved = []

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        loop = hg.Feedback(hg.TS[int])
        saved.append((loop, value))
        loop.bind(value)
        return loop.output

    hg.run_graph(graph, [1])
    with pytest.raises(RuntimeError, match="outside its graph construction"):
        saved[-1][0].output
    with pytest.raises(RuntimeError, match="outside its graph construction"):
        saved[-1][0].bind(saved[-1][1])


def test_feedback_handles_and_run_state_are_independent():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        first = hg.Feedback(hg.TS[int], "first")
        second = hg.Feedback(hg.TS[int], "second")
        result = first.output + second.output
        first.bind(value)
        second.bind(value * 10)
        return result

    assert hg.run_graph(graph, [2, 3]) == [None, 22, 33]
    assert hg.run_graph(graph, [4]) == [None, 44]
