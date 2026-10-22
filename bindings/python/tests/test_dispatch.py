from typing import TypeVar

import pytest

import bg


def test_dispatch_selects_python_class_implementations_and_falls_back():
    class Base:
        pass

    class Derived(Base):
        pass

    @bg.dispatch
    def choose(value: bg.TS[Base]) -> bg.TS[str]: ...

    @choose.overload
    @bg.compute_node
    def base(value: bg.TS[Base]) -> bg.TS[str]:
        return "base"

    @choose.overload
    @bg.compute_node
    def derived(value: bg.TS[Derived]) -> bg.TS[str]:
        return "derived"

    @bg.graph
    def graph(value: bg.TS[Base]) -> bg.TS[str]:
        return choose(value)

    assert bg.test_graph(graph, [Base(), Derived(), Base()]) == ["base", "derived", "base"]


def test_dispatch_accepts_explicit_graph_list_and_named_scalar_argument():
    class Base:
        pass

    class Derived(Base):
        pass

    @bg.graph
    def base(value: bg.TS[Base], amount: bg.Scalar[int]) -> bg.TS[int]:
        return bg.operators.const_(amount)

    @bg.graph
    def derived(value: bg.TS[Derived], amount: bg.Scalar[int]) -> bg.TS[int]:
        return bg.operators.const_(amount + 10)

    def signature(value: bg.TS[Base], amount: bg.Scalar[int] = 3) -> bg.TS[int]: ...

    choose = bg.dispatch(signature, [base, derived])

    @bg.graph
    def graph(value: bg.TS[Base]) -> bg.TS[int]:
        return choose(value=value, amount=5)

    assert bg.test_graph(graph, [Derived(), Base(), Derived()]) == [15, 5, 15]


def test_dispatch_specializes_a_generic_contract():
    class Base:
        pass

    class Derived(Base):
        pass

    SERIES = TypeVar("SERIES", bound=bg.TimeSeries)

    @bg.dispatch
    def choose(value: SERIES) -> bg.TS[str]: ...

    specialized = choose[bg.TS[Base]]

    @choose.overload
    @bg.compute_node
    def base(value: bg.TS[Base]) -> bg.TS[str]:
        return "base"

    @choose.overload
    @bg.compute_node
    def derived(value: bg.TS[Derived]) -> bg.TS[str]:
        return "derived"

    @bg.graph
    def graph(value: bg.TS[Base]) -> bg.TS[str]:
        return specialized(value)

    assert bg.test_graph(graph, [Base(), Derived()]) == ["base", "derived"]


def test_dispatch_selects_a_runtime_type_tuple():
    class Base:
        pass

    class Derived(Base):
        pass

    @bg.dispatch
    def choose(first: bg.TS[Base], second: bg.TS[Base]) -> bg.TS[str]: ...

    @choose.overload
    @bg.compute_node
    def fallback(first: bg.TS[Base], second: bg.TS[Base]) -> bg.TS[str]:
        return "fallback"

    @choose.overload
    @bg.compute_node
    def both_derived(first: bg.TS[Derived], second: bg.TS[Derived]) -> bg.TS[str]:
        return "both derived"

    @bg.graph
    def graph(first: bg.TS[Base], second: bg.TS[Base]) -> bg.TS[str]:
        return choose(first, second)

    assert bg.test_graph(graph, [Derived(), Derived(), Base()], [Derived(), Base(), Derived()]) == [
        "both derived", "fallback", "fallback"
    ]


def test_dispatch_without_a_fallback_rejects_an_unmatched_runtime_type():
    class Base:
        pass

    class Derived(Base):
        pass

    @bg.dispatch
    def choose(value: bg.TS[Base]) -> bg.TS[str]: ...

    @choose.overload
    @bg.compute_node
    def derived(value: bg.TS[Derived]) -> bg.TS[str]:
        return "derived"

    @bg.graph
    def graph(value: bg.TS[Base]) -> bg.TS[str]:
        return choose(value)

    with pytest.raises(IndexError, match="dispatch input types do not match"):
        bg.test_graph(graph, [Derived(), Base()])


def test_dispatch_selects_derived_schema_values():
    @bg.schema
    class Base:
        label: str

    @bg.schema
    class Derived(Base):
        number: int

    @bg.dispatch
    def choose(value: bg.TS[bg.Bundle[Base]]) -> bg.TS[str]: ...

    @choose.overload
    @bg.compute_node
    def base(value: bg.TS[bg.Bundle[Base]]) -> bg.TS[str]:
        return "base"

    @choose.overload
    @bg.compute_node
    def derived(value: bg.TS[bg.Bundle[Derived]]) -> bg.TS[str]:
        return "derived"

    @bg.graph
    def graph(value: bg.TS[bg.Bundle[Base]]) -> bg.TS[str]:
        return choose(value)

    assert bg.test_graph(graph, [Derived("a", 1), Base("b"), Derived("c", 2)]) == [
        "derived", "base", "derived"
    ]


def test_dispatch_supports_outputless_nodes():
    class Base:
        pass

    class Derived(Base):
        pass

    calls = []

    @bg.dispatch
    def record(value: bg.TS[Base]) -> None: ...

    @record.overload
    @bg.compute_node
    def base(value: bg.TS[Base]) -> None:
        calls.append("base")

    @record.overload
    @bg.compute_node
    def derived(value: bg.TS[Derived]) -> None:
        calls.append("derived")

    @bg.graph
    def graph(value: bg.TS[Base]) -> bg.TS[Base]:
        assert record(value) is None
        return value

    values = [Derived(), Base()]
    assert bg.test_graph(graph, values) == values
    assert calls == ["derived", "base"]


def test_dispatch_rejects_undecorated_implementation():
    class Base:
        pass

    @bg.dispatch
    def choose(value: bg.TS[Base]) -> bg.TS[str]: ...

    with pytest.raises(TypeError, match="dispatch implementations"):
        choose.overload(lambda value: value)


def test_dispatch_requires_an_applicable_implementation():
    class Base:
        pass

    @bg.dispatch
    def choose(value: bg.TS[Base]) -> bg.TS[str]: ...

    @bg.graph
    def graph(value: bg.TS[Base]) -> bg.TS[str]:
        return choose(value)

    with pytest.raises(ValueError, match="at least one implementation"):
        bg.test_graph(graph, [Base()])


def test_dispatch_rejects_an_input_without_value_covariance():
    @bg.dispatch
    def choose(value: bg.TS[int]) -> bg.TS[str]: ...

    @choose.overload
    @bg.compute_node
    def implementation(value: bg.TS[int]) -> bg.TS[str]:
        return "value"

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[str]:
        return choose(value)

    with pytest.raises(ValueError, match="covariance-capable"):
        bg.test_graph(graph, [1])


def test_dispatch_preserves_python_wiring_exception():
    class Base:
        pass

    error = RuntimeError("implementation wiring failed")

    @bg.dispatch
    def choose(value: bg.TS[Base]) -> bg.TS[str]: ...

    @choose.overload
    @bg.graph
    def implementation(value: bg.TS[Base]) -> bg.TS[str]:
        raise error

    @bg.graph
    def graph(value: bg.TS[Base]) -> bg.TS[str]:
        return choose(value)

    with pytest.raises(RuntimeError) as caught:
        bg.test_graph(graph, [Base()])
    assert caught.value is error
