from typing import Optional

import pytest

import hg


def test_bundle_construction_and_python_class_round_trip():
    @hg.schema
    class Quote:
        bid: float
        ask: Optional[float]

    quote = Quote(2.0, ask=3.0)
    missing_ask = Quote(4.0)

    @hg.source_node
    def source() -> hg.TS[hg.Bundle[Quote]]:
        return quote

    @hg.compute_node
    def read(value: hg.TS[hg.Bundle[Quote]]) -> hg.TS[float]:
        observed = value.value
        assert type(observed) is Quote
        return observed.bid + (observed.ask or 0.0)

    @hg.graph
    def graph() -> hg.TS[float]:
        return read(source())

    assert hg.run_graph(graph) == [5.0]
    assert missing_ask.ask is None


def test_bundle_covariance_retains_the_actual_derived_python_class():
    @hg.schema
    class BaseQuote:
        bid: float

    @hg.schema
    class Quote(BaseQuote):
        ask: int

    @hg.source_node
    def source() -> hg.TS[hg.Bundle[Quote]]:
        return Quote(1.5, ask=2)

    @hg.compute_node
    def read(value: hg.TS[hg.Bundle[BaseQuote]]) -> hg.TS[int]:
        observed = value.value
        assert type(observed) is Quote
        return int(observed.bid) + observed.ask

    @hg.graph
    def graph() -> hg.TS[int]:
        return read(source())

    assert hg.run_graph(graph) == [3]


def test_tsb_projects_scalar_fields_and_preserves_explicit_time_series_fields():
    @hg.schema
    class Pair:
        value: int
        text: hg.TS[str]

    @hg.compute_node
    def make(value: hg.TS[int], text: hg.TS[str]) -> hg.TSB[Pair]:
        return {"value": value.value, "text": text.value}

    @hg.compute_node
    def read(value: hg.TSB[Pair]) -> hg.TS[int]:
        return value.value.value + len(value.text.value)

    @hg.graph
    def graph(value: hg.TS[int], text: hg.TS[str]) -> hg.TS[int]:
        return read(make(value, text))

    assert hg.run_graph(graph, [2], ["abc"]) == [5]


def test_bundle_rejects_time_series_schema_fields():
    @hg.schema
    class Mixed:
        value: int
        text: hg.TS[str]

    with pytest.raises(TypeError, match="scalar Bundle schema"):
        hg._native_type(Mixed)


def test_optional_recursive_schema_field_round_trips():
    @hg.schema
    class Node:
        value: int
        child: Optional["Node"]

    node = Node(1, child=Node(2))

    @hg.source_node
    def source() -> hg.TS[hg.Bundle[Node]]:
        return node

    @hg.compute_node
    def read(value: hg.TS[hg.Bundle[Node]]) -> hg.TS[int]:
        observed = value.value
        assert type(observed) is Node
        assert type(observed.child) is Node
        return observed.value + observed.child.value

    @hg.graph
    def graph() -> hg.TS[int]:
        return read(source())

    assert hg.run_graph(graph) == [3]


def test_recursive_schema_projects_and_wires_as_tsb():
    @hg.schema
    class Node:
        value: int
        child: Optional["Node"]

    child = Node(2)

    @hg.compute_node
    def make(trigger: hg.TS[int]) -> hg.TSB[Node]:
        return {"value": trigger.value, "child": child}

    @hg.compute_node
    def read(value: hg.TSB[Node]) -> hg.TS[int]:
        observed = value.child.value
        assert type(observed) is Node
        return value.value.value + observed.value

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return read(make(trigger))

    assert hg.run_graph(graph, [1]) == [3]


def test_required_recursive_schema_field_is_rejected():
    with pytest.raises(ValueError, match="recursive cycle"):

        @hg.schema
        class Node:
            child: "Node"

    @hg.schema
    class Node:
        value: int
        child: Optional["Node"]

    @hg.source_node
    def source() -> hg.TS[hg.Bundle[Node]]:
        return Node(1)

    @hg.compute_node
    def read(value: hg.TS[hg.Bundle[Node]]) -> hg.TS[int]:
        observed = value.value
        assert type(observed) is Node
        assert observed.child is None
        return observed.value

    @hg.graph
    def graph() -> hg.TS[int]:
        return read(source())

    assert hg.run_graph(graph) == [1]


def test_python_schema_conversion_does_not_call_custom_constructor():
    @hg.schema
    class Entry:
        value: int

        def __init__(self, value: int):
            self.value = value * 10

    entry = Entry(3)

    @hg.source_node
    def source() -> hg.TS[hg.Bundle[Entry]]:
        return entry

    @hg.compute_node
    def read(value: hg.TS[hg.Bundle[Entry]]) -> hg.TS[int]:
        observed = value.value
        assert type(observed) is Entry
        return observed.value

    @hg.graph
    def graph() -> hg.TS[int]:
        return read(source())

    assert hg.run_graph(graph) == [30]


def test_const_schema_with_nested_list_strings_and_bytes_survives_repeated_wiring():
    @hg.schema
    class Detail:
        labels: tuple[str, ...]
        payload: bytes

    @hg.schema
    class Envelope:
        detail: Detail
        count: int

    value = Envelope(detail=Detail(labels=("first", "second"), payload=b"\x00nested\xff"), count=3)

    @hg.graph
    def graph() -> hg.TS[hg.Bundle[Envelope]]:
        return hg.operators.const_(value)

    for _ in range(2):
        observed = hg.run_graph(graph, capture_output=True)[0]
        assert type(observed) is Envelope
        assert observed.detail.labels == ("first", "second")
        assert observed.detail.payload == b"\x00nested\xff"
        assert observed.count == 3


def test_schema_value_converts_inside_scalar_dictionary():
    @hg.schema
    class Quote:
        bid: float

    quotes = {7: Quote(2.5)}

    @hg.source_node
    def source() -> hg.TS[dict[int, Quote]]:
        return quotes

    @hg.compute_node
    def read(value: hg.TS[dict[int, Quote]]) -> hg.TS[float]:
        observed = value.value
        assert type(observed[7]) is Quote
        return observed[7].bid

    @hg.graph
    def graph() -> hg.TS[float]:
        return read(source())

    assert hg.run_graph(graph) == [2.5]


def test_native_schema_export_supports_tsb_and_scalar_projections():
    from hg._hg_test_extension import export_native_pair_schema, native_pair_consumer

    class NativePair:
        def __init__(self, value):
            self.value = value

    export_native_pair_schema(NativePair)

    @hg.source_node
    def scalar_source() -> hg.TS[NativePair]:
        return NativePair(4)

    @hg.compute_node
    def scalar_read(value: hg.TS[hg.Bundle[NativePair]]) -> hg.TS[int]:
        return value.value.value

    @hg.graph
    def scalar_graph() -> hg.TS[int]:
        return scalar_read(scalar_source())

    assert hg.run_graph(scalar_graph) == [4]

    @hg.compute_node
    def make(value: hg.TS[int]) -> hg.TSB[NativePair]:
        return {"value": value.value}

    @hg.compute_node
    def read(value: hg.TSB[NativePair]) -> hg.TS[int]:
        return value.value.value

    @hg.graph
    def time_series_graph(value: hg.TS[int]) -> hg.TS[int]:
        return read(make(value))

    assert hg.run_graph(time_series_graph, [7]) == [7]

    @hg.graph
    def native_consumer_graph(value: hg.TS[int]) -> hg.TS[int]:
        return native_pair_consumer(make(value))

    assert hg.run_graph(native_consumer_graph, [9]) == [9]


def test_native_tsb_only_schema_export_does_not_require_scalar_projection():
    from hg._hg_test_extension import export_native_tsb_schema, native_pair_consumer

    class NativeBase:
        pass

    export_native_tsb_schema(NativeBase)

    @hg.schema
    class NativeChild(NativeBase):
        text: hg.TS[str]

    @hg.compute_node
    def make(value: hg.TS[int], text: hg.TS[str]) -> hg.TSB[NativeChild]:
        return {"value": value.value, "text": text.value}

    @hg.compute_node
    def read(value: hg.TSB[NativeChild]) -> hg.TS[str]:
        return f"{value.value.value}:{value.text.value}"

    @hg.graph
    def graph(value: hg.TS[int], text: hg.TS[str]) -> hg.TS[str]:
        return read(make(value, text))

    assert hg.run_graph(graph, [3], ["native"]) == ["3:native"]

    @hg.graph
    def native_consumer_graph(value: hg.TS[int], text: hg.TS[str]) -> hg.TS[int]:
        return native_pair_consumer(make(value, text))

    assert hg.run_graph(native_consumer_graph, [3], ["native"]) == [3]
    with pytest.raises(TypeError, match="scalar Bundle schema"):
        hg._native_type(NativeChild)
    with pytest.raises(TypeError, match=r"Bundle\[\.\.\.\]"):
        hg.Bundle[NativeBase]
    with pytest.raises(TypeError, match="scalar Bundle schema"):
        hg.Bundle[NativeChild]


def test_native_schema_export_is_idempotent_and_can_add_scalar_projection():
    from hg._hg_test_extension import export_native_pair_schema, export_native_tsb_schema

    class NativePair:
        pass

    export_native_tsb_schema(NativePair)
    export_native_tsb_schema(NativePair)
    export_native_pair_schema(NativePair)
    export_native_pair_schema(NativePair)
    export_native_tsb_schema(NativePair)

    @hg.source_node
    def source() -> hg.TS[hg.Bundle[NativePair]]:
        return NativePair(value=12)

    @hg.compute_node
    def read(value: hg.TS[hg.Bundle[NativePair]]) -> hg.TS[int]:
        return value.value.value

    @hg.graph
    def graph() -> hg.TS[int]:
        return read(source())

    assert hg.run_graph(graph) == [12]


def test_native_scalar_export_installs_constructor_with_scalar_optional_fields():
    from hg._hg_test_extension import export_native_optional_schema, export_native_optional_tsb_schema

    class NativeOptional:
        pass

    export_native_optional_tsb_schema(NativeOptional)
    assert "__init__" not in NativeOptional.__dict__
    export_native_optional_schema(NativeOptional)

    assert NativeOptional._schema_field_names == ("value",)
    assert NativeOptional._schema_optional_fields == {"value": True}
    assert NativeOptional().value is None

    @hg.source_node
    def source() -> hg.TS[hg.Bundle[NativeOptional]]:
        return NativeOptional()

    @hg.compute_node
    def read(value: hg.TS[hg.Bundle[NativeOptional]]) -> hg.TS[bool]:
        return value.value.value is None

    @hg.graph
    def graph() -> hg.TS[bool]:
        return read(source())

    assert hg.run_graph(graph) == [True]


def test_native_schema_export_rejects_conflicting_rebinding():
    from hg._hg_test_extension import export_conflicting_tsb_schema, export_native_pair_schema

    class NativePair:
        pass

    export_native_pair_schema(NativePair)
    with pytest.raises(ValueError, match="different native schema binding"):
        export_conflicting_tsb_schema(NativePair)
