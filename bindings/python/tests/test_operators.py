import pytest

import bg


extension = pytest.importorskip("bg._bg_test_extension")


def test_const_operator_accepts_positional_and_named_scalars():
    @bg.graph
    def positional() -> bg.TS[int]:
        return bg.operators.const_(42)

    @bg.graph
    def named() -> bg.TS[int]:
        return bg.operators.const_(value=43)

    assert bg.test_graph(positional) == [42]
    assert bg.test_graph(named) == [43]


def test_scalar_inference_uses_python_type_order():
    class Count(int):
        pass

    @bg.graph
    def boolean() -> bg.TS[bool]:
        return bg.operators.const_(True)

    @bg.graph
    def integer_subclass() -> bg.TS[int]:
        return bg.operators.const_(Count(7))

    assert bg.test_graph(boolean) == [True]
    assert bg.test_graph(integer_subclass) == [7]


def test_extension_can_register_a_python_scalar_type():
    class Measure:
        def __float__(self):
            return 1.5

    assert extension.register_double_type(Measure)

    @bg.graph
    def value() -> bg.TS[float]:
        return bg.operators.const_(Measure())

    assert bg.test_graph(value) == [1.5]


def test_getitem_operator_and_wiring_point_syntax_select_list_items():
    @bg.graph
    def explicit() -> bg.TS[int]:
        return bg.operators.getitem(collection=extension.make_list(), key=1)

    @bg.graph
    def syntax() -> bg.TS[int]:
        return extension.make_list()[0]

    assert bg.test_graph(explicit) == [20]
    assert bg.test_graph(syntax) == [10]


def test_operator_reports_normal_overload_diagnostics():
    @bg.graph
    def invalid() -> bg.TS[int]:
        return bg.operators.getitem(bg.operators.const_(1), 0)

    with pytest.raises(ValueError, match="Cannot resolve operator getitem") as raised:
        bg.test_graph(invalid)
    assert any("callback invalid at invalid[0]" in note for note in raised.value.__notes__)


def test_extension_can_export_existing_and_sink_operators():
    @bg.graph
    def constant() -> bg.TS[int]:
        return extension.const_(44)

    @bg.graph
    def sink() -> None:
        assert extension.discard(bg.operators.const_(1)) is None

    assert bg.test_graph(constant) == [44]
    bg.test_graph(sink)


def test_opt_requires_an_optional_python_node_receiver():
    received = []

    @bg.node
    def observe(value: int | None) -> None:
        received.append(value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        value = extension.invalidate_on_zero(trigger)
        observe(value=bg.opt(value))
        return value

    assert bg.test_graph(graph, [1, 0, 2]) == [1, None, 2]
    assert received == [1, None, 2]


def test_opt_rejects_a_required_python_node_receiver_with_a_clear_diagnostic():
    @bg.node
    def observe(value: int) -> None:
        pass

    @bg.graph
    def graph(value: bg.TS[int]) -> None:
        observe(bg.opt(value))

    with pytest.raises(ValueError, match="an opt-qualified argument requires an optional input parameter"):
        bg.test_graph(graph, [1])


def test_opt_rejects_a_required_python_graph_receiver():
    @bg.graph
    def required(value: bg.TS[int]) -> bg.TS[int]:
        return value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return required(bg.opt(value))

    with pytest.raises(ValueError, match="an opt-qualified argument requires an optional input parameter"):
        bg.test_graph(graph, [1])


def test_opt_is_preserved_through_python_operator_calls_and_cache_keys():
    @bg.node
    def add(lhs: int, rhs: int) -> int:
        return lhs + rhs

    @bg.graph
    def required_then_optional(value: bg.TS[int]) -> bg.TS[int]:
        required = extension.optional_choice(value=value)
        optional = extension.optional_choice(value=bg.opt(value))
        return add(required, optional)

    @bg.graph
    def optional_then_required(value: bg.TS[int]) -> bg.TS[int]:
        optional = extension.optional_choice(value=bg.opt(value))
        required = extension.optional_choice(value=value)
        return add(optional, required)

    assert bg.test_graph(required_then_optional, [3]) == [36]
    assert bg.test_graph(optional_then_required, [3]) == [36]


def test_opt_rejects_values_without_time_series_identity():
    with pytest.raises(TypeError, match="time-series wiring point"):
        bg.opt(1)
    with pytest.raises(TypeError, match="time-series wiring point"):
        bg.opt(None)


def test_opt_preserves_wiring_point_lifetime():
    optional_points = []

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        optional = bg.opt(value)
        optional_points.append(optional)
        assert value.type_name == optional.type_name
        assert value.has_output and optional.has_output
        assert bg.opt(optional).type_name == value.type_name
        return value

    assert bg.test_graph(graph, [3]) == [3]
    with pytest.raises(RuntimeError, match="outside its graph construction"):
        _ = optional_points[0].has_output
    with pytest.raises(RuntimeError, match="outside its graph construction"):
        bg.opt(optional_points[0])

    @bg.graph
    def use_expired(value: bg.TS[int]) -> bg.TS[int]:
        return extension.optional_choice(optional_points[0])

    with pytest.raises(RuntimeError, match="outside its graph construction"):
        bg.test_graph(use_expired, [3])


def test_repeated_opt_qualifier_is_accepted_by_an_optional_operator_input():
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return extension.optional_choice(value=bg.opt(bg.opt(value)))

    assert bg.test_graph(graph, [0]) == [20]


def test_opt_qualified_graph_output_keeps_normal_output_semantics():
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return bg.opt(value)

    assert bg.test_graph(graph, [1, 2]) == [1, 2]
