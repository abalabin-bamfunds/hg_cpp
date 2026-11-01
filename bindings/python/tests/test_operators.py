import pytest

import hg


extension = pytest.importorskip("hg._hg_test_extension")


def test_const_operator_accepts_positional_and_named_scalars():
    @hg.graph
    def positional() -> hg.TS[int]:
        return hg.operators.const_(42)

    @hg.graph
    def named() -> hg.TS[int]:
        return hg.operators.const_(value=43)

    assert hg.run_graph(positional) == [42]
    assert hg.run_graph(named) == [43]


def test_scalar_inference_uses_python_type_order():
    class Count(int):
        pass

    @hg.graph
    def boolean() -> hg.TS[bool]:
        return hg.operators.const_(True)

    @hg.graph
    def integer_subclass() -> hg.TS[int]:
        return hg.operators.const_(Count(7))

    assert hg.run_graph(boolean) == [True]
    assert hg.run_graph(integer_subclass) == [7]


def test_extension_can_register_a_python_scalar_type():
    class Measure:
        def __float__(self):
            return 1.5

    class DerivedMeasure(Measure):
        pass

    assert extension.register_double_type(Measure)

    @hg.graph
    def value() -> hg.TS[float]:
        return hg.operators.const_(DerivedMeasure())

    assert hg.run_graph(value) == [1.5]


def test_getitem_operator_and_wiring_point_syntax_select_list_items():
    @hg.graph
    def explicit() -> hg.TS[int]:
        return hg.operators.getitem(collection=extension.make_list(), key=1)

    @hg.graph
    def syntax() -> hg.TS[int]:
        return extension.make_list()[0]

    assert hg.run_graph(explicit) == [20]
    assert hg.run_graph(syntax) == [10]


def test_operator_reports_normal_overload_diagnostics():
    @hg.graph
    def invalid() -> hg.TS[int]:
        return hg.operators.getitem(hg.operators.const_(1), 0)

    with pytest.raises(ValueError, match="Cannot resolve operator getitem") as raised:
        hg.run_graph(invalid)
    assert any("callback invalid at invalid[0]" in note for note in raised.value.__notes__)


def test_extension_can_export_existing_and_sink_operators():
    @hg.graph
    def constant() -> hg.TS[int]:
        return extension.const_(44)

    @hg.graph
    def sink() -> None:
        assert extension.discard(hg.operators.const_(1)) is None

    assert hg.run_graph(constant) == [44]
    hg.run_graph(sink)


def test_opt_requires_an_optional_python_node_receiver():
    received = []

    @hg.node
    def observe(value: int | None) -> None:
        received.append(value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        value = extension.invalidate_on_zero(trigger)
        observe(value=hg.opt(value))
        return value

    assert hg.run_graph(graph, [1, 0, 2]) == [1, None, 2]
    assert received == [1, None, 2]


def test_opt_rejects_a_required_python_node_receiver_with_a_clear_diagnostic():
    @hg.node
    def observe(value: int) -> None:
        pass

    @hg.graph
    def graph(value: hg.TS[int]) -> None:
        observe(hg.opt(value))

    with pytest.raises(ValueError, match="an opt-qualified argument requires an optional input parameter"):
        hg.run_graph(graph, [1])


def test_opt_rejects_a_required_python_graph_receiver():
    @hg.graph
    def required(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return required(hg.opt(value))

    with pytest.raises(ValueError, match="an opt-qualified argument requires an optional input parameter"):
        hg.run_graph(graph, [1])


def test_opt_is_preserved_through_python_operator_calls_and_cache_keys():
    @hg.node
    def add(lhs: int, rhs: int) -> int:
        return lhs + rhs

    @hg.graph
    def required_then_optional(value: hg.TS[int]) -> hg.TS[int]:
        required = extension.optional_choice(value=value)
        optional = extension.optional_choice(value=hg.opt(value))
        return add(required, optional)

    @hg.graph
    def optional_then_required(value: hg.TS[int]) -> hg.TS[int]:
        optional = extension.optional_choice(value=hg.opt(value))
        required = extension.optional_choice(value=value)
        return add(optional, required)

    assert hg.run_graph(required_then_optional, [3]) == [36]
    assert hg.run_graph(optional_then_required, [3]) == [36]


def test_opt_rejects_values_without_time_series_identity():
    with pytest.raises(TypeError, match="time-series wiring point"):
        hg.opt(1)
    with pytest.raises(TypeError, match="time-series wiring point"):
        hg.opt(None)


def test_opt_preserves_wiring_point_lifetime():
    optional_points = []

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        optional = hg.opt(value)
        optional_points.append(optional)
        assert value.type_name == optional.type_name
        assert value.has_output and optional.has_output
        assert hg.opt(optional).type_name == value.type_name
        return value

    assert hg.run_graph(graph, [3]) == [3]
    with pytest.raises(RuntimeError, match="outside its graph construction"):
        _ = optional_points[0].has_output
    with pytest.raises(RuntimeError, match="outside its graph construction"):
        hg.opt(optional_points[0])

    @hg.graph
    def use_expired(value: hg.TS[int]) -> hg.TS[int]:
        return extension.optional_choice(optional_points[0])

    with pytest.raises(RuntimeError, match="outside its graph construction"):
        hg.run_graph(use_expired, [3])


def test_repeated_opt_qualifier_is_accepted_by_an_optional_operator_input():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return extension.optional_choice(value=hg.opt(hg.opt(value)))

    assert hg.run_graph(graph, [0]) == [20]


def test_opt_qualified_graph_output_keeps_normal_output_semantics():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return hg.opt(value)

    assert hg.run_graph(graph, [1, 2]) == [1, 2]
