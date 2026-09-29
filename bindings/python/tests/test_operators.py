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

    with pytest.raises(ValueError, match="Cannot resolve operator getitem"):
        bg.test_graph(invalid)


def test_extension_can_export_existing_and_sink_operators():
    @bg.graph
    def constant() -> bg.TS[int]:
        return extension.const_(44)

    @bg.graph
    def sink() -> None:
        assert extension.discard(bg.operators.const_(1)) is None

    assert bg.test_graph(constant) == [44]
    bg.test_graph(sink)
