import pytest

import bg


def test_node_accepts_scalar_arguments_and_runs_at_graph_start():
    @bg.node
    def add(lhs: int, rhs: int = 2) -> int:
        return lhs + rhs

    @bg.graph
    def graph() -> bg.TS[int]:
        return add(40)

    assert bg.test_graph(graph) == [42]


def test_node_receives_plain_values_for_time_series_and_scalar_inputs():
    @bg.node
    def add(value: int, offset: int) -> int:
        assert type(value) is int
        assert type(offset) is int
        return value + offset

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return add(value, 3)

    assert bg.test_graph(graph, [1, 2, None, 4]) == [4, 5, None, 7]


def test_node_accepts_multiple_time_series_values():
    @bg.node
    def add(lhs: int, rhs: int) -> int:
        return lhs + rhs

    @bg.graph
    def graph(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return add(lhs, rhs)

    assert bg.test_graph(graph, [1, None, 3], [10, 20, 30]) == [11, 21, 33]


def test_node_optional_time_series_input_receives_none_when_invalid():
    @bg.node
    def add(trigger: int, value: int | None) -> int:
        return -trigger if value is None else trigger + value

    @bg.graph
    def graph(trigger: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return add(trigger, value)

    assert bg.test_graph(graph, [1, 2], [None, 4]) == [-1, 6]


def test_node_optional_scalar_input_receives_none():
    @bg.node
    def add(value: int | None, offset: int) -> int:
        return (value or 0) + offset

    @bg.graph
    def graph() -> bg.TS[int]:
        return add(None, 4)

    assert bg.test_graph(graph) == [4]


def test_node_does_not_run_at_start_for_an_invalid_optional_time_series_input():
    @bg.node
    def echo(value: int | None) -> int:
        return -1 if value is None else value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return echo(value)

    assert bg.test_graph(graph, [None, 5]) == [None, 5]


def test_node_sink_receives_time_series_values():
    seen = []

    @bg.node
    def observe(value: int) -> None:
        seen.append(value)

    @bg.source_node
    def source() -> bg.TS[int]:
        return 0

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        observe(value)
        return source()

    assert bg.test_graph(graph, [2, 3]) == [0, None]
    assert seen == [2, 3]


def test_node_scalar_only_sink_runs_once_at_graph_start():
    seen = []

    @bg.node
    def observe(value: int) -> None:
        seen.append(value)

    @bg.source_node
    def source() -> bg.TS[int]:
        return 1

    @bg.graph
    def graph() -> bg.TS[int]:
        observe(7)
        return source()

    assert bg.test_graph(graph) == [1]
    assert seen == [7]


def test_node_rejects_time_series_input_annotations():
    with pytest.raises(TypeError, match="scalar annotations"):
        @bg.node
        def invalid(value: bg.TS[int]) -> int:
            return value.value


def test_node_rejects_time_series_output_annotations():
    with pytest.raises(TypeError, match="return annotations must be scalar"):
        @bg.node
        def invalid(value: int) -> bg.TS[int]:
            return value
