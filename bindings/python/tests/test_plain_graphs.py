from typing import Optional

import pytest

import hgraph as hg


def test_plain_graph_input_accepts_a_scalar_or_time_series():
    @hg.node
    def identity(value: int) -> int:
        return value

    @hg.graph
    def graph(value: int) -> int:
        return identity(value)

    assert hg.run_graph(graph, [1, 2, None, 4]) == [1, 2, None, 4]


def test_plain_graph_inputs_can_be_mixed_with_explicit_scalar_and_time_series_inputs():
    @hg.compute_node
    def combine(value: hg.TS[int], scale: hg.TS[int], offset: int) -> hg.TS[int]:
        return value.value * scale.value + offset

    @hg.graph
    def mixed(value: int, scale: hg.TS[int], offset: hg.Scalar[int]) -> int:
        return combine(value, scale, offset)

    @hg.graph
    def graph(value: hg.TS[int], scale: hg.TS[int]) -> hg.TS[int]:
        return mixed(value, scale, offset=3)

    assert hg.run_graph(graph, [2, 4], [5, 10]) == [13, 43]


def test_plain_graph_accepts_a_scalar_argument_during_wiring():
    @hg.graph
    def add(left: int, right: int) -> int:
        return left + right

    @hg.graph
    def graph() -> int:
        return add(12, 30)

    assert hg.run_graph(graph) == [42]


def test_scalar_graph_parameter_rejects_a_time_series():
    @hg.graph
    def scalar_only(value: hg.Scalar[int]) -> int:
        return value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return scalar_only(value)

    with pytest.raises(ValueError, match="cannot wire Python graph"):
        hg.run_graph(graph, [1])


def test_scalar_graph_parameter_accepts_a_scalar():
    @hg.graph
    def scalar_only(value: hg.Scalar[int]) -> int:
        return value

    @hg.graph
    def graph() -> int:
        return scalar_only(7)

    assert hg.run_graph(graph) == [7]


def test_scalar_annotation_rejects_a_time_series_type():
    with pytest.raises(TypeError, match="Scalar.*not TS"):

        @hg.graph
        def graph(value: hg.Scalar[hg.TS[int]]) -> hg.TS[int]:
            return value


def test_scalar_annotation_is_not_a_graph_return_type():
    with pytest.raises(TypeError, match="Scalar.*graph input"):

        @hg.graph
        def graph() -> hg.Scalar[int]:
            return 1


def test_plain_graph_inputs_forward_through_nested_graphs():
    @hg.node
    def identity(value: int) -> int:
        return value

    @hg.graph
    def inner(value: int) -> int:
        return identity(value)

    @hg.graph
    def outer(value: int) -> int:
        return inner(value)

    assert hg.run_graph(outer, [3, 5]) == [3, 5]


def test_plain_graph_parameters_support_defaults_and_named_arguments():
    @hg.compute_node
    def add(value: hg.TS[int], offset: int) -> hg.TS[int]:
        return value.value + offset

    @hg.graph
    def inner(value: int, offset: int = 2) -> int:
        return add(value, offset)

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return inner(offset=4, value=value)

    assert hg.run_graph(graph, [1, 3]) == [5, 7]


def test_optional_plain_graph_input_accepts_an_absent_scalar():
    @hg.node
    def choose(value: int | None) -> int:
        return 9 if value is None else value

    @hg.graph
    def inner(value: int | None) -> int:
        return choose(value)

    @hg.graph
    def graph() -> int:
        return inner(None)

    assert hg.run_graph(graph) == [9]


def test_optional_plain_graph_input_accepts_a_scalar_value():
    @hg.node
    def choose(value: int | None) -> int:
        return 9 if value is None else value

    @hg.graph
    def inner(value: int | None) -> int:
        return choose(value)

    @hg.graph
    def graph() -> int:
        return inner(5)

    assert hg.run_graph(graph) == [5]


def test_optional_plain_graph_default_can_be_absent():
    @hg.node
    def choose(value: int | None = None) -> int:
        return 9 if value is None else value

    @hg.graph
    def inner(value: int | None = None) -> int:
        return choose(value)

    @hg.graph
    def graph() -> int:
        return inner()

    assert hg.run_graph(graph) == [9]


def test_optional_scalar_annotation_accepts_only_an_optional_scalar():
    @hg.node
    def choose(value: int | None) -> int:
        return 9 if value is None else value

    @hg.graph
    def inner(value: Optional[hg.Scalar[int]]) -> int:
        return choose(value)

    @hg.graph
    def scalar_call() -> int:
        return inner(None)

    @hg.graph
    def time_series_call(value: hg.TS[int]) -> hg.TS[int]:
        return inner(hg.opt(value))

    assert hg.run_graph(scalar_call) == [9]
    with pytest.raises(ValueError, match="cannot wire Python graph"):
        hg.run_graph(time_series_call, [4])


def test_scalar_optional_annotation_also_accepts_an_absent_scalar():
    @hg.node
    def choose(value: int | None) -> int:
        return 9 if value is None else value

    @hg.graph
    def inner(value: hg.Scalar[Optional[int]]) -> int:
        return choose(value)

    @hg.graph
    def graph() -> int:
        return inner(None)

    assert hg.run_graph(graph) == [9]


def test_optional_plain_graph_input_forwards_opt_to_an_optional_node():
    @hg.node
    def observe(value: int | None) -> int:
        return 0 if value is None else value

    @hg.graph
    def graph(value: int | None) -> int:
        return observe(value)

    assert hg.run_graph(graph, [None, 4, None, 6]) == [None, 4, None, 6]


def test_optional_plain_graph_input_cannot_forward_to_required_node():
    @hg.node
    def required(value: int) -> int:
        return value

    @hg.graph
    def inner(value: int | None) -> int:
        return required(value)

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return inner(value)

    with pytest.raises(ValueError, match="optional input parameter"):
        hg.run_graph(graph, [4])


def test_plain_scalar_graph_return_is_lifted_to_a_constant_time_series():
    @hg.graph
    def graph() -> int:
        return 42

    assert hg.run_graph(graph) == [42]


def test_plain_graph_sink_uses_a_plain_scalar_parameter():
    received = []

    @hg.compute_node
    def sink(value: hg.TS[int]) -> None:
        received.append(value.value)

    @hg.graph
    def graph(value: int) -> None:
        sink(value)

    assert hg.run_graph(graph, [1, 2]) is None
    assert received == [1, 2]


def test_plain_dictionary_parameter_promotes_to_ts_of_dictionary():
    @hg.node
    def size(value: dict[str, int]) -> int:
        return len(value)

    @hg.graph
    def graph(value: dict[str, int]) -> int:
        return size(value)

    assert hg.run_graph(graph, [{"a": 1}, {"a": 1, "b": 2}]) == [1, 2]


def test_plain_dictionary_scalar_is_lifted_to_ts_of_dictionary():
    @hg.graph
    def identity(value: dict[str, int]) -> dict[str, int]:
        return value

    @hg.graph
    def graph() -> dict[str, int]:
        return identity({"a": 1, "b": 2})

    assert hg.run_graph(graph) == [{"a": 1, "b": 2}]


def test_plain_graph_return_rejects_an_incompatible_scalar():
    @hg.graph
    def graph() -> int:
        return "wrong"

    with pytest.raises(TypeError, match="expected an integer for hgraph.i64"):
        hg.run_graph(graph)


def test_explicit_time_series_return_still_requires_a_wiring_point():
    @hg.graph
    def graph() -> hg.TS[int]:
        return 42

    with pytest.raises(ValueError, match="must return a wiring point"):
        hg.run_graph(graph)


def test_plain_graph_branch_works_in_switch():
    @hg.compute_node
    def identity(value: hg.TS[int]) -> hg.TS[int]:
        return value.value

    @hg.graph
    def first(value: int) -> int:
        return identity(value)

    @hg.graph
    def second(value: int) -> int:
        return identity(value)

    @hg.graph
    def select(key: hg.TS[int], value: int) -> int:
        return hg.switch_(key, {1: first, 2: second}, value)

    assert hg.run_graph(select, [1, 2], [10, 20]) == [10, 20]


def test_plain_graph_branch_works_in_map():
    @hg.compute_node
    def make_values(value: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: value.value, 2: value.value * 10}

    @hg.compute_node
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left.value + right.value

    @hg.graph
    def branch(key: hg.TS[int], value: int) -> int:
        return add(key, value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.map(branch, make_values(trigger), __key_arg__=0)[2]

    assert hg.run_graph(graph, [1, 3]) == [12, 32]


def test_scalar_annotation_restricts_python_node_input():
    @hg.node
    def identity(value: hg.Scalar[int]) -> int:
        return value

    @hg.graph
    def scalar_call() -> int:
        return identity(5)

    @hg.graph
    def time_series_call(value: hg.TS[int]) -> hg.TS[int]:
        return identity(value)

    assert hg.run_graph(scalar_call) == [5]
    with pytest.raises(ValueError, match="cannot wire Python node"):
        hg.run_graph(time_series_call, [5])


def test_python_graph_output_uses_normal_time_series_type_checking():
    @hg.graph
    def graph() -> hg.TS[int]:
        return hg.operators.const_(1.5)

    with pytest.raises(ValueError, match="does not satisfy its resolved output type"):
        hg.run_graph(graph)
