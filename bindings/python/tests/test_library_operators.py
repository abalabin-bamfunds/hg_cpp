import pytest

import bg


def test_add_operator_accepts_double_scalars_and_time_series():
    @bg.graph
    def scalar_add() -> bg.TS[float]:
        return bg.operators.add(1.25, 2.5)

    @bg.graph
    def mixed_add(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(value, 2.5)

    @bg.graph
    def series_add(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(lhs, rhs)

    @bg.graph
    def keyword_add(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(rhs=rhs, lhs=lhs)

    assert bg.test_graph(scalar_add) == [3.75]
    assert bg.test_graph(mixed_add, [1.0, 2.0]) == [3.5, 4.5]
    assert bg.test_graph(series_add, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]
    assert bg.test_graph(keyword_add, [1.0], [10.0]) == [11.0]


def test_wiring_point_addition_uses_the_native_operator():
    @bg.node
    def passthrough(value: float) -> float:
        return value

    @bg.graph
    def add_nested(lhs: bg.TS[float], rhs: float) -> bg.TS[float]:
        return lhs + rhs

    @bg.graph
    def graph(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        left = passthrough(lhs)
        right = passthrough(rhs)
        forward = left + right
        return add_nested(forward, 2.0)

    @bg.graph
    def reflected(value: bg.TS[float]) -> bg.TS[float]:
        return 2.0 + value

    assert bg.test_graph(graph, [1.0, 2.0], [10.0, 20.0]) == [13.0, 24.0]
    assert bg.test_graph(reflected, [3.0, 4.0]) == [5.0, 6.0]


def test_add_operator_accepts_values_dereferenced_from_references():
    @bg.compute_node
    def forward(value: bg.REF[bg.TS[float]]) -> bg.REF[bg.TS[float]]:
        return value.value

    @bg.graph
    def graph(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return forward(lhs) + forward(rhs)

    assert bg.test_graph(graph, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]


def test_reflected_add_preserves_operand_order_and_native_diagnostics():
    @bg.graph
    def invalid(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add("unsupported", value)

    direct = invalid

    @bg.graph
    def invalid(value: bg.TS[float]) -> bg.TS[float]:
        return "unsupported" + value

    with pytest.raises(ValueError, match="Cannot resolve operator add") as raised:
        bg.test_graph(direct, [1.0])

    with pytest.raises(ValueError, match="Cannot resolve operator add") as reflected:
        bg.test_graph(invalid, [1.0])

    assert "Arguments: (lhs: string, rhs:" in str(raised.value)
    assert str(reflected.value) == str(raised.value)


def test_add_operator_preserves_required_input_validity():
    extension = pytest.importorskip("bg._bg_test_extension")

    @bg.compute_node
    def to_double(value: bg.TS[int]) -> bg.TS[float]:
        return float(value.value)

    @bg.graph
    def graph(value: bg.TS[int], rhs: bg.TS[float]) -> bg.TS[float]:
        lhs = to_double(extension.invalidate_on_zero(value))
        return lhs + rhs

    assert bg.test_graph(graph, [1, 0, 3], [10.0, 20.0, 30.0]) == [11.0, None, 33.0]


def test_add_does_not_schedule_on_start_for_an_initially_invalid_input():
    @bg.graph
    def graph(value: bg.TS[float]) -> bg.TS[float]:
        return value + 1.0

    assert bg.test_graph(graph, [None, 2.0]) == [None, 3.0]


def test_default_operator_is_available_with_native_fallback_behavior():
    @bg.graph
    def graph(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.default_(value, fallback=5.0)

    assert bg.test_graph(graph, [None, 1.0, 3.0]) == [5.0, 1.0, 3.0]


def test_add_rejects_optional_input_qualifiers_without_an_optional_overload():
    @bg.graph
    def graph(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(bg.opt(value), 1.0)

    with pytest.raises(ValueError, match="an opt-qualified argument requires an optional input parameter"):
        bg.test_graph(graph, [1.0])


def test_add_reports_native_overload_diagnostics_for_unsupported_types():
    @bg.graph
    def graph(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.add(lhs, rhs)

    with pytest.raises(ValueError, match="Cannot resolve operator add"):
        bg.test_graph(graph, [1], [2])

    @bg.graph
    def mixed(lhs: bg.TS[float], rhs: bg.TS[int]) -> bg.TS[float]:
        return bg.operators.add(lhs, rhs)

    with pytest.raises(ValueError, match="Cannot resolve operator add"):
        bg.test_graph(mixed, [1.0], [2])


def test_operator_namespace_reexports_existing_higher_order_apis():
    assert bg.operators.map is bg.map
    assert bg.operators.mesh is bg.mesh
    assert bg.operators.switch_ is bg.switch_


def test_addition_checks_wiring_point_lifetime():
    saved_points = []

    @bg.graph
    def save(value: bg.TS[float]) -> bg.TS[float]:
        saved_points.append(value)
        return value

    assert bg.test_graph(save, [1.0]) == [1.0]

    @bg.graph
    def use_expired(value: bg.TS[float]) -> bg.TS[float]:
        return 1.0 + saved_points[0]

    with pytest.raises(RuntimeError, match="outside its graph construction"):
        bg.test_graph(use_expired, [2.0])
