import pytest

import hg


def test_node_accepts_scalar_arguments_and_runs_at_graph_start():
    @hg.node
    def add(lhs: int, rhs: int = 2) -> int:
        return lhs + rhs

    @hg.graph
    def graph() -> hg.TS[int]:
        return add(40)

    assert hg.run_graph(graph) == [42]


def test_node_receives_plain_values_for_time_series_and_scalar_inputs():
    @hg.node
    def add(value: int, offset: int) -> int:
        assert type(value) is int
        assert type(offset) is int
        return value + offset

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return add(value, 3)

    assert hg.run_graph(graph, [1, 2, None, 4]) == [4, 5, None, 7]


def test_node_accepts_multiple_time_series_values():
    @hg.node
    def add(lhs: int, rhs: int) -> int:
        return lhs + rhs

    @hg.graph
    def graph(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return add(lhs, rhs)

    assert hg.run_graph(graph, [1, None, 3], [10, 20, 30]) == [11, 21, 33]


def test_node_optional_time_series_input_receives_none_when_invalid():
    @hg.node
    def add(trigger: int, value: int | None) -> int:
        return -trigger if value is None else trigger + value

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return add(trigger, value)

    assert hg.run_graph(graph, [1, 2], [None, 4]) == [-1, 6]


def test_node_optional_scalar_input_receives_none():
    @hg.node
    def add(value: int | None, offset: int) -> int:
        return (value or 0) + offset

    @hg.graph
    def graph() -> hg.TS[int]:
        return add(None, 4)

    assert hg.run_graph(graph) == [4]


def test_node_does_not_run_at_start_for_an_invalid_optional_time_series_input():
    @hg.node
    def echo(value: int | None) -> int:
        return -1 if value is None else value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return echo(value)

    assert hg.run_graph(graph, [None, 5]) == [None, 5]


def test_node_sink_receives_time_series_values():
    seen = []

    @hg.node
    def observe(value: int) -> None:
        seen.append(value)

    @hg.source_node
    def source() -> hg.TS[int]:
        return 0

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        observe(value)
        return source()

    assert hg.run_graph(graph, [2, 3]) == [0, None]
    assert seen == [2, 3]


def test_node_scalar_only_sink_runs_once_at_graph_start():
    seen = []

    @hg.node
    def observe(value: int) -> None:
        seen.append(value)

    @hg.source_node
    def source() -> hg.TS[int]:
        return 1

    @hg.graph
    def graph() -> hg.TS[int]:
        observe(7)
        return source()

    assert hg.run_graph(graph) == [1]
    assert seen == [7]


def test_node_rejects_time_series_input_annotations():
    with pytest.raises(TypeError, match="scalar annotations"):
        @hg.node
        def invalid(value: hg.TS[int]) -> int:
            return value.value


def test_node_rejects_time_series_output_annotations():
    with pytest.raises(TypeError, match="return annotations must be scalar"):
        @hg.node
        def invalid(value: int) -> hg.TS[int]:
            return value
