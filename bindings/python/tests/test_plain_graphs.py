from typing import Optional

import pytest

import bg


def test_plain_graph_input_accepts_a_scalar_or_time_series():
    @bg.node
    def identity(value: int) -> int:
        return value

    @bg.graph
    def graph(value: int) -> int:
        return identity(value)

    assert bg.run_graph(graph, [1, 2, None, 4]) == [1, 2, None, 4]


def test_plain_graph_inputs_can_be_mixed_with_explicit_scalar_and_time_series_inputs():
    @bg.compute_node
    def combine(value: bg.TS[int], scale: bg.TS[int], offset: int) -> bg.TS[int]:
        return value.value * scale.value + offset

    @bg.graph
    def mixed(value: int, scale: bg.TS[int], offset: bg.Scalar[int]) -> int:
        return combine(value, scale, offset)

    @bg.graph
    def graph(value: bg.TS[int], scale: bg.TS[int]) -> bg.TS[int]:
        return mixed(value, scale, offset=3)

    assert bg.run_graph(graph, [2, 4], [5, 10]) == [13, 43]


def test_plain_graph_accepts_a_scalar_argument_during_wiring():
    @bg.graph
    def add(left: int, right: int) -> int:
        return left + right

    @bg.graph
    def graph() -> int:
        return add(12, 30)

    assert bg.run_graph(graph) == [42]


def test_scalar_graph_parameter_rejects_a_time_series():
    @bg.graph
    def scalar_only(value: bg.Scalar[int]) -> int:
        return value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return scalar_only(value)

    with pytest.raises(ValueError, match="cannot wire Python graph"):
        bg.run_graph(graph, [1])


def test_scalar_graph_parameter_accepts_a_scalar():
    @bg.graph
    def scalar_only(value: bg.Scalar[int]) -> int:
        return value

    @bg.graph
    def graph() -> int:
        return scalar_only(7)

    assert bg.run_graph(graph) == [7]


def test_scalar_annotation_rejects_a_time_series_type():
    with pytest.raises(TypeError, match="Scalar.*not TS"):

        @bg.graph
        def graph(value: bg.Scalar[bg.TS[int]]) -> bg.TS[int]:
            return value


def test_scalar_annotation_is_not_a_graph_return_type():
    with pytest.raises(TypeError, match="Scalar.*graph input"):

        @bg.graph
        def graph() -> bg.Scalar[int]:
            return 1


def test_plain_graph_inputs_forward_through_nested_graphs():
    @bg.node
    def identity(value: int) -> int:
        return value

    @bg.graph
    def inner(value: int) -> int:
        return identity(value)

    @bg.graph
    def outer(value: int) -> int:
        return inner(value)

    assert bg.run_graph(outer, [3, 5]) == [3, 5]


def test_plain_graph_parameters_support_defaults_and_named_arguments():
    @bg.compute_node
    def add(value: bg.TS[int], offset: int) -> bg.TS[int]:
        return value.value + offset

    @bg.graph
    def inner(value: int, offset: int = 2) -> int:
        return add(value, offset)

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return inner(offset=4, value=value)

    assert bg.run_graph(graph, [1, 3]) == [5, 7]


def test_optional_plain_graph_input_accepts_an_absent_scalar():
    @bg.node
    def choose(value: int | None) -> int:
        return 9 if value is None else value

    @bg.graph
    def inner(value: int | None) -> int:
        return choose(value)

    @bg.graph
    def graph() -> int:
        return inner(None)

    assert bg.run_graph(graph) == [9]


def test_optional_plain_graph_input_accepts_a_scalar_value():
    @bg.node
    def choose(value: int | None) -> int:
        return 9 if value is None else value

    @bg.graph
    def inner(value: int | None) -> int:
        return choose(value)

    @bg.graph
    def graph() -> int:
        return inner(5)

    assert bg.run_graph(graph) == [5]


def test_optional_plain_graph_default_can_be_absent():
    @bg.node
    def choose(value: int | None = None) -> int:
        return 9 if value is None else value

    @bg.graph
    def inner(value: int | None = None) -> int:
        return choose(value)

    @bg.graph
    def graph() -> int:
        return inner()

    assert bg.run_graph(graph) == [9]


def test_optional_scalar_annotation_accepts_only_an_optional_scalar():
    @bg.node
    def choose(value: int | None) -> int:
        return 9 if value is None else value

    @bg.graph
    def inner(value: Optional[bg.Scalar[int]]) -> int:
        return choose(value)

    @bg.graph
    def scalar_call() -> int:
        return inner(None)

    @bg.graph
    def time_series_call(value: bg.TS[int]) -> bg.TS[int]:
        return inner(bg.opt(value))

    assert bg.run_graph(scalar_call) == [9]
    with pytest.raises(ValueError, match="cannot wire Python graph"):
        bg.run_graph(time_series_call, [4])


def test_scalar_optional_annotation_also_accepts_an_absent_scalar():
    @bg.node
    def choose(value: int | None) -> int:
        return 9 if value is None else value

    @bg.graph
    def inner(value: bg.Scalar[Optional[int]]) -> int:
        return choose(value)

    @bg.graph
    def graph() -> int:
        return inner(None)

    assert bg.run_graph(graph) == [9]


def test_optional_plain_graph_input_forwards_opt_to_an_optional_node():
    @bg.node
    def observe(value: int | None) -> int:
        return 0 if value is None else value

    @bg.graph
    def graph(value: int | None) -> int:
        return observe(value)

    assert bg.run_graph(graph, [None, 4, None, 6]) == [None, 4, None, 6]


def test_optional_plain_graph_input_cannot_forward_to_required_node():
    @bg.node
    def required(value: int) -> int:
        return value

    @bg.graph
    def inner(value: int | None) -> int:
        return required(value)

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return inner(value)

    with pytest.raises(ValueError, match="optional input parameter"):
        bg.run_graph(graph, [4])


def test_plain_scalar_graph_return_is_lifted_to_a_constant_time_series():
    @bg.graph
    def graph() -> int:
        return 42

    assert bg.run_graph(graph) == [42]


def test_plain_graph_sink_uses_a_plain_scalar_parameter():
    received = []

    @bg.compute_node
    def sink(value: bg.TS[int]) -> None:
        received.append(value.value)

    @bg.graph
    def graph(value: int) -> None:
        sink(value)

    assert bg.run_graph(graph, [1, 2]) is None
    assert received == [1, 2]
    assert received == [1, 2]


def test_plain_dictionary_parameter_promotes_to_ts_of_dictionary():
    @bg.node
    def size(value: dict[str, int]) -> int:
        return len(value)

    @bg.graph
    def graph(value: dict[str, int]) -> int:
        return size(value)

    assert bg.run_graph(graph, [{"a": 1}, {"a": 1, "b": 2}]) == [1, 2]


def test_plain_dictionary_scalar_is_lifted_to_ts_of_dictionary():
    @bg.graph
    def identity(value: dict[str, int]) -> dict[str, int]:
        return value

    @bg.graph
    def graph() -> dict[str, int]:
        return identity({"a": 1, "b": 2})

    assert bg.run_graph(graph) == [{"a": 1, "b": 2}]


def test_plain_graph_return_rejects_an_incompatible_scalar():
    @bg.graph
    def graph() -> int:
        return "wrong"

    with pytest.raises(TypeError, match="expected an integer for bg.i64"):
        bg.run_graph(graph)


def test_explicit_time_series_return_still_requires_a_wiring_point():
    @bg.graph
    def graph() -> bg.TS[int]:
        return 42

    with pytest.raises(ValueError, match="must return a wiring point"):
        bg.run_graph(graph)


def test_plain_graph_branch_works_in_switch():
    @bg.compute_node
    def identity(value: bg.TS[int]) -> bg.TS[int]:
        return value.value

    @bg.graph
    def first(value: int) -> int:
        return identity(value)

    @bg.graph
    def second(value: int) -> int:
        return identity(value)

    @bg.graph
    def select(key: bg.TS[int], value: int) -> int:
        return bg.switch_(key, {1: first, 2: second}, value)

    assert bg.run_graph(select, [1, 2], [10, 20]) == [10, 20]


def test_plain_graph_branch_works_in_map():
    @bg.compute_node
    def make_values(value: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {1: value.value, 2: value.value * 10}

    @bg.compute_node
    def add(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return left.value + right.value

    @bg.graph
    def branch(key: bg.TS[int], value: int) -> int:
        return add(key, value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return bg.map(branch, make_values(trigger), __key_arg__=0)[2]

    assert bg.run_graph(graph, [1, 3]) == [12, 32]


def test_scalar_annotation_restricts_python_node_input():
    @bg.node
    def identity(value: bg.Scalar[int]) -> int:
        return value

    @bg.graph
    def scalar_call() -> int:
        return identity(5)

    @bg.graph
    def time_series_call(value: bg.TS[int]) -> bg.TS[int]:
        return identity(value)

    assert bg.run_graph(scalar_call) == [5]
    with pytest.raises(ValueError, match="cannot wire Python node"):
        bg.run_graph(time_series_call, [5])


def test_python_graph_output_uses_normal_time_series_type_checking():
    @bg.graph
    def graph() -> bg.TS[int]:
        return bg.operators.const_(1.5)

    with pytest.raises(ValueError, match="does not satisfy its resolved output type"):
        bg.run_graph(graph)
