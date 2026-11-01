from typing import TypeVar

import pytest

import hg


def test_dispatch_selects_python_class_implementations_and_falls_back():
    class Base:
        pass

    class Derived(Base):
        pass

    @hg.dispatch
    def choose(value: hg.TS[Base]) -> hg.TS[str]: ...

    @choose.overload
    @hg.compute_node
    def base(value: hg.TS[Base]) -> hg.TS[str]:
        return "base"

    @choose.overload
    @hg.compute_node
    def derived(value: hg.TS[Derived]) -> hg.TS[str]:
        return "derived"

    @hg.graph
    def graph(value: hg.TS[Base]) -> hg.TS[str]:
        return choose(value)

    assert hg.run_graph(graph, [Base(), Derived(), Base()]) == ["base", "derived", "base"]


def test_dispatch_accepts_explicit_graph_list_and_named_scalar_argument():
    class Base:
        pass

    class Derived(Base):
        pass

    @hg.graph
    def base(value: hg.TS[Base], amount: hg.Scalar[int]) -> hg.TS[int]:
        return hg.operators.const_(amount)

    @hg.graph
    def derived(value: hg.TS[Derived], amount: hg.Scalar[int]) -> hg.TS[int]:
        return hg.operators.const_(amount + 10)

    def signature(value: hg.TS[Base], amount: hg.Scalar[int] = 3) -> hg.TS[int]: ...

    choose = hg.dispatch(signature, [base, derived])

    @hg.graph
    def graph(value: hg.TS[Base]) -> hg.TS[int]:
        return choose(value=value, amount=5)

    assert hg.run_graph(graph, [Derived(), Base(), Derived()]) == [15, 5, 15]


def test_dispatch_specializes_a_generic_contract():
    class Base:
        pass

    class Derived(Base):
        pass

    SERIES = TypeVar("SERIES", bound=hg.TimeSeries)

    @hg.dispatch
    def choose(value: SERIES) -> hg.TS[str]: ...

    specialized = choose[hg.TS[Base]]

    @choose.overload
    @hg.compute_node
    def base(value: hg.TS[Base]) -> hg.TS[str]:
        return "base"

    @choose.overload
    @hg.compute_node
    def derived(value: hg.TS[Derived]) -> hg.TS[str]:
        return "derived"

    @hg.graph
    def graph(value: hg.TS[Base]) -> hg.TS[str]:
        return specialized(value)

    assert hg.run_graph(graph, [Base(), Derived()]) == ["base", "derived"]


def test_dispatch_selects_a_runtime_type_tuple():
    class Base:
        pass

    class Derived(Base):
        pass

    @hg.dispatch
    def choose(first: hg.TS[Base], second: hg.TS[Base]) -> hg.TS[str]: ...

    @choose.overload
    @hg.compute_node
    def fallback(first: hg.TS[Base], second: hg.TS[Base]) -> hg.TS[str]:
        return "fallback"

    @choose.overload
    @hg.compute_node
    def both_derived(first: hg.TS[Derived], second: hg.TS[Derived]) -> hg.TS[str]:
        return "both derived"

    @hg.graph
    def graph(first: hg.TS[Base], second: hg.TS[Base]) -> hg.TS[str]:
        return choose(first, second)

    assert hg.run_graph(graph, [Derived(), Derived(), Base()], [Derived(), Base(), Derived()]) == [
        "both derived", "fallback", "fallback"
    ]


def test_dispatch_without_a_fallback_rejects_an_unmatched_runtime_type():
    class Base:
        pass

    class Derived(Base):
        pass

    @hg.dispatch
    def choose(value: hg.TS[Base]) -> hg.TS[str]: ...

    @choose.overload
    @hg.compute_node
    def derived(value: hg.TS[Derived]) -> hg.TS[str]:
        return "derived"

    @hg.graph
    def graph(value: hg.TS[Base]) -> hg.TS[str]:
        return choose(value)

    with pytest.raises(IndexError, match="dispatch input types do not match"):
        hg.run_graph(graph, [Derived(), Base()])


def test_dispatch_selects_derived_schema_values():
    @hg.schema
    class Base:
        label: str

    @hg.schema
    class Derived(Base):
        number: int

    @hg.dispatch
    def choose(value: hg.TS[hg.Bundle[Base]]) -> hg.TS[str]: ...

    @choose.overload
    @hg.compute_node
    def base(value: hg.TS[hg.Bundle[Base]]) -> hg.TS[str]:
        return "base"

    @choose.overload
    @hg.compute_node
    def derived(value: hg.TS[hg.Bundle[Derived]]) -> hg.TS[str]:
        return "derived"

    @hg.graph
    def graph(value: hg.TS[hg.Bundle[Base]]) -> hg.TS[str]:
        return choose(value)

    assert hg.run_graph(graph, [Derived("a", 1), Base("b"), Derived("c", 2)]) == [
        "derived", "base", "derived"
    ]


def test_dispatch_supports_outputless_nodes():
    class Base:
        pass

    class Derived(Base):
        pass

    calls = []

    @hg.dispatch
    def record(value: hg.TS[Base]) -> None: ...

    @record.overload
    @hg.compute_node
    def base(value: hg.TS[Base]) -> None:
        calls.append("base")

    @record.overload
    @hg.compute_node
    def derived(value: hg.TS[Derived]) -> None:
        calls.append("derived")

    @hg.graph
    def graph(value: hg.TS[Base]) -> hg.TS[Base]:
        assert record(value) is None
        return value

    values = [Derived(), Base()]
    assert hg.run_graph(graph, values) == values
    assert calls == ["derived", "base"]


def test_dispatch_rejects_undecorated_implementation():
    class Base:
        pass

    @hg.dispatch
    def choose(value: hg.TS[Base]) -> hg.TS[str]: ...

    with pytest.raises(TypeError, match="dispatch implementations"):
        choose.overload(lambda value: value)


def test_dispatch_requires_an_applicable_implementation():
    class Base:
        pass

    @hg.dispatch
    def choose(value: hg.TS[Base]) -> hg.TS[str]: ...

    @hg.graph
    def graph(value: hg.TS[Base]) -> hg.TS[str]:
        return choose(value)

    with pytest.raises(ValueError, match="at least one implementation"):
        hg.run_graph(graph, [Base()])


def test_dispatch_rejects_an_input_without_value_covariance():
    @hg.dispatch
    def choose(value: hg.TS[int]) -> hg.TS[str]: ...

    @choose.overload
    @hg.compute_node
    def implementation(value: hg.TS[int]) -> hg.TS[str]:
        return "value"

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[str]:
        return choose(value)

    with pytest.raises(ValueError, match="covariance-capable"):
        hg.run_graph(graph, [1])


def test_dispatch_preserves_python_wiring_exception():
    class Base:
        pass

    error = RuntimeError("implementation wiring failed")

    @hg.dispatch
    def choose(value: hg.TS[Base]) -> hg.TS[str]: ...

    @choose.overload
    @hg.graph
    def implementation(value: hg.TS[Base]) -> hg.TS[str]:
        raise error

    @hg.graph
    def graph(value: hg.TS[Base]) -> hg.TS[str]:
        return choose(value)

    with pytest.raises(RuntimeError) as caught:
        hg.run_graph(graph, [Base()])
    assert caught.value is error
