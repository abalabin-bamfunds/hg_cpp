from datetime import datetime

import pytest

import hgraph as hg


def test_output_view_reads_previous_scalar_tick_and_does_not_publish_none():
    observed = []

    @hg.compute_node
    def accumulate(value: hg.TS[int], output: hg.Output = None) -> hg.TS[int]:
        previous = output.value if output.valid else 0
        observed.append(
            (
                value.value,
                output.valid,
                previous,
                output.modified,
                output.delta_value,
                output.last_modified_time,
            )
        )
        if value.value == 2:
            return None
        return previous + value.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return accumulate(value)

    assert hg.run_graph(graph, [1, None, 2, 3]) == [1, None, None, 4]
    assert [entry[:5] for entry in observed] == [
        (1, False, 0, False, None),
        (2, True, 1, False, None),
        (3, True, 1, False, None),
    ]
    assert all(isinstance(entry[5], datetime) for entry in observed)
    assert observed[1][5] == observed[2][5]


def test_output_view_works_with_node_scalar_promotion():
    observed = []

    @hg.node
    def accumulate(value: int, output: hg.Output = None) -> int:
        previous = output.value if output.valid else 0
        observed.append((output.valid, previous, output.modified, output.delta_value))
        return previous + value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return accumulate(value)

    assert hg.run_graph(graph, [2, 3]) == [2, 5]
    assert observed == [(False, 0, False, None), (True, 2, False, None)]


def test_output_view_reads_previous_collection_value():
    observed = []

    @hg.compute_node
    def update(value: hg.TS[int], output: hg.Output = None) -> hg.TSD[int, hg.TS[int]]:
        if output.valid:
            item = output[5]
            observed.append((len(output), item.valid, item.value, output.modified, output.delta_value))
        else:
            observed.append((len(output), False, None, output.modified, output.delta_value))
        return {5: value.value}

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return update(value)

    actual = hg.run_graph(graph, [10, 20, 30])

    assert actual == [{5: 10}, {5: 20}, {5: 30}]
    assert observed == [
        (0, False, None, False, None),
        (1, True, 10, False, None),
        (1, True, 20, False, None),
    ]


def test_output_view_is_available_to_source_and_lifecycle_callbacks():
    observed = []
    retained = []

    @hg.source_node
    def source(output: hg.Output = None) -> hg.TS[int]:
        observed.append(("evaluate", output.valid))
        retained.append(output)
        return 7

    @source.start
    def start(output: hg.Output = None):
        observed.append(("start", output.valid))

    @source.stop
    def stop(output: hg.Output = None):
        observed.append(("stop", output.valid, output.value))

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    assert hg.run_graph(graph) == [7]
    assert observed == [("start", False), ("evaluate", False), ("stop", True, 7)]
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = retained[0].valid


def test_invalidation_callback_reads_previous_output_view():
    observed = []

    @hg.compute_node
    def identity(value: hg.TS[int], output: hg.Output = None) -> hg.TS[int]:
        return value.value

    @identity.on_invalidation
    def invalidated(output: hg.Output = None) -> int:
        observed.append((output.valid, output.value, output.modified, output.delta_value))
        return 99

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return identity(value)

    assert hg.run_graph(graph, [7, hg.INVALID]) == [7, 99]
    assert observed == [(True, 7, False, None)]


def test_output_injection_requires_a_node_output_and_is_rejected_for_generators():
    with pytest.raises(TypeError, match="requires a Python node with a time-series output"):

        @hg.node
        def sink(output: hg.Output = None) -> None:
            pass

    with pytest.raises(TypeError, match="generators cannot use hgraph.Output"):

        @hg.generator
        def source(output: hg.Output = None) -> hg.TS[int]:
            yield 0, 1
