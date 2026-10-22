from typing import Optional

import pytest

import bg


def test_bundle_construction_and_python_class_round_trip():
    @bg.schema
    class Quote:
        bid: float
        ask: Optional[float]

    quote = Quote(2.0, ask=3.0)
    missing_ask = Quote(4.0)

    @bg.source_node
    def source() -> bg.TS[bg.Bundle[Quote]]:
        return quote

    @bg.compute_node
    def read(value: bg.TS[bg.Bundle[Quote]]) -> bg.TS[float]:
        observed = value.value
        assert type(observed) is Quote
        return observed.bid + (observed.ask or 0.0)

    @bg.graph
    def graph() -> bg.TS[float]:
        return read(source())

    assert bg.test_graph(graph) == [5.0]
    assert missing_ask.ask is None


def test_bundle_covariance_retains_the_actual_derived_python_class():
    @bg.schema
    class BaseQuote:
        bid: float

    @bg.schema
    class Quote(BaseQuote):
        ask: int

    @bg.source_node
    def source() -> bg.TS[bg.Bundle[Quote]]:
        return Quote(1.5, ask=2)

    @bg.compute_node
    def read(value: bg.TS[bg.Bundle[BaseQuote]]) -> bg.TS[int]:
        observed = value.value
        assert type(observed) is Quote
        return int(observed.bid) + observed.ask

    @bg.graph
    def graph() -> bg.TS[int]:
        return read(source())

    assert bg.test_graph(graph) == [3]


def test_tsb_projects_scalar_fields_and_preserves_explicit_time_series_fields():
    @bg.schema
    class Pair:
        value: int
        text: bg.TS[str]

    @bg.compute_node
    def make(value: bg.TS[int], text: bg.TS[str]) -> bg.TSB[Pair]:
        return {"value": value.value, "text": text.value}

    @bg.compute_node
    def read(value: bg.TSB[Pair]) -> bg.TS[int]:
        return value.value.value + len(value.text.value)

    @bg.graph
    def graph(value: bg.TS[int], text: bg.TS[str]) -> bg.TS[int]:
        return read(make(value, text))

    assert bg.test_graph(graph, [2], ["abc"]) == [5]


def test_bundle_rejects_time_series_schema_fields():
    @bg.schema
    class Mixed:
        value: int
        text: bg.TS[str]

    with pytest.raises(TypeError, match="scalar Bundle schema"):
        bg._native_type(Mixed)


def test_optional_recursive_schema_field_round_trips():
    @bg.schema
    class Node:
        value: int
        child: Optional["Node"]

    node = Node(1, child=Node(2))

    @bg.source_node
    def source() -> bg.TS[bg.Bundle[Node]]:
        return node

    @bg.compute_node
    def read(value: bg.TS[bg.Bundle[Node]]) -> bg.TS[int]:
        observed = value.value
        assert type(observed) is Node
        assert type(observed.child) is Node
        return observed.value + observed.child.value

    @bg.graph
    def graph() -> bg.TS[int]:
        return read(source())

    assert bg.test_graph(graph) == [3]


def test_recursive_schema_projects_and_wires_as_tsb():
    @bg.schema
    class Node:
        value: int
        child: Optional["Node"]

    child = Node(2)

    @bg.compute_node
    def make(trigger: bg.TS[int]) -> bg.TSB[Node]:
        return {"value": trigger.value, "child": child}

    @bg.compute_node
    def read(value: bg.TSB[Node]) -> bg.TS[int]:
        observed = value.child.value
        assert type(observed) is Node
        return value.value.value + observed.value

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return read(make(trigger))

    assert bg.test_graph(graph, [1]) == [3]


def test_required_recursive_schema_field_is_rejected():
    with pytest.raises(ValueError, match="recursive cycle"):

        @bg.schema
        class Node:
            child: "Node"

    @bg.schema
    class Node:
        value: int
        child: Optional["Node"]

    @bg.source_node
    def source() -> bg.TS[bg.Bundle[Node]]:
        return Node(1)

    @bg.compute_node
    def read(value: bg.TS[bg.Bundle[Node]]) -> bg.TS[int]:
        observed = value.value
        assert type(observed) is Node
        assert observed.child is None
        return observed.value

    @bg.graph
    def graph() -> bg.TS[int]:
        return read(source())

    assert bg.test_graph(graph) == [1]


def test_python_schema_conversion_does_not_call_custom_constructor():
    @bg.schema
    class Entry:
        value: int

        def __init__(self, value: int):
            self.value = value * 10

    entry = Entry(3)

    @bg.source_node
    def source() -> bg.TS[bg.Bundle[Entry]]:
        return entry

    @bg.compute_node
    def read(value: bg.TS[bg.Bundle[Entry]]) -> bg.TS[int]:
        observed = value.value
        assert type(observed) is Entry
        return observed.value

    @bg.graph
    def graph() -> bg.TS[int]:
        return read(source())

    assert bg.test_graph(graph) == [30]


def test_schema_value_converts_inside_scalar_dictionary():
    @bg.schema
    class Quote:
        bid: float

    quotes = {7: Quote(2.5)}

    @bg.source_node
    def source() -> bg.TS[dict[int, Quote]]:
        return quotes

    @bg.compute_node
    def read(value: bg.TS[dict[int, Quote]]) -> bg.TS[float]:
        observed = value.value
        assert type(observed[7]) is Quote
        return observed[7].bid

    @bg.graph
    def graph() -> bg.TS[float]:
        return read(source())

    assert bg.test_graph(graph) == [2.5]
