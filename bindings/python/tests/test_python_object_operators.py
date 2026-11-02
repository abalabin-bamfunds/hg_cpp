import pytest

import hgraph as hg


def test_getitem_infers_python_object_result_for_scalar_key():
    class Indexed:
        def __getitem__(self, key: int) -> str:
            return f"item-{key}"

    value = Indexed()

    @hg.graph
    def graph(value: hg.TS[Indexed]) -> hg.TS[str]:
        return hg.operators.getitem(value, 3)

    assert hg.run_graph(graph, [value])[0] == "item-3"


def test_getitem_accepts_time_series_key_for_python_object():
    class Indexed:
        def __getitem__(self, key: int) -> str:
            return f"item-{key}"

    value = Indexed()

    @hg.graph
    def graph(value: hg.TS[Indexed], key: hg.TS[int]) -> hg.TS[str]:
        return value[key]

    assert hg.run_graph(graph, [value, value], [1, 2]) == ["item-1", "item-2"]


def test_getitem_accepts_unhashable_python_object_key():
    class Key:
        __hash__ = None

    class Indexed:
        def __getitem__(self, key: Key) -> int:
            return 7

    @hg.graph
    def graph(value: hg.TS[Indexed], key: hg.TS[Key]) -> hg.TS[int]:
        return value[key]

    assert hg.run_graph(graph, [Indexed()], [Key()]) == [7]


def test_getitem_accepts_explicit_output_type_without_annotation():
    class Indexed:
        def __getitem__(self, key):
            return key * 2

    value = Indexed()

    @hg.graph
    def graph(value: hg.TS[Indexed]) -> hg.TS[int]:
        return hg.operators.getitem[int](value, 4)

    assert hg.run_graph(graph, [value]) == [8]


def test_getitem_explicit_output_type_overrides_annotation():
    class Indexed:
        def __getitem__(self, key: int) -> int:
            return "value"

    @hg.graph
    def graph(value: hg.TS[Indexed]) -> hg.TS[str]:
        return hg.operators.getitem[str](value, 0)

    assert hg.run_graph(graph, [Indexed()]) == ["value"]


def test_python_object_getitem_result_supports_covariant_output_type():
    class Base:
        pass

    class Derived(Base):
        pass

    derived = Derived()

    class Indexed:
        def __getitem__(self, key: int) -> Base:
            return derived

    @hg.graph
    def graph(value: hg.TS[Indexed]) -> hg.TS[Base]:
        return value[0]

    result = hg.run_graph(graph, [Indexed()])[0]
    assert type(result) is Derived
    assert result is derived


def test_python_object_operators_do_not_match_unsupported_builtin_scalars():
    @hg.graph
    def getitem_graph(value: hg.TS[int]) -> hg.TS[int]:
        return value[0]

    @hg.graph
    def getattr_graph(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.getattr(value, "real")

    @hg.graph
    def explicit_getattr_graph(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.getattr[int](value, "real")

    with pytest.raises(ValueError, match="Cannot resolve operator getitem"):
        hg.run_graph(getitem_graph, [1])

    with pytest.raises(ValueError, match="Cannot resolve operator getattr"):
        hg.run_graph(getattr_graph, [1])

    with pytest.raises(ValueError, match="Cannot resolve operator getattr"):
        hg.run_graph(explicit_getattr_graph, [1])


def test_getattr_infers_annotated_field_and_property_types():
    class Record:
        name: str

        @property
        def count(self) -> int:
            return 2

    record = Record()
    record.name = "record"

    @hg.graph
    def read_name(value: hg.TS[Record]) -> hg.TS[str]:
        return hg.operators.getattr(value, hg.symbol("name"))

    @hg.graph
    def read_count(value: hg.TS[Record]) -> hg.TS[int]:
        return hg.operators.getattr(value, hg.symbol("count"))

    assert hg.run_graph(read_name, [record]) == ["record"]
    assert hg.run_graph(read_count, [record]) == [2]


def test_getattr_accepts_explicit_output_type_and_plain_string_name():
    class Record:
        @property
        def count(self):
            return 2

    @hg.graph
    def graph(value: hg.TS[Record]) -> hg.TS[int]:
        return hg.operators.getattr[int](value, "count")

    assert hg.run_graph(graph, [Record()]) == [2]


def test_getattr_rejects_non_string_attribute_name():
    class Record:
        pass

    @hg.graph
    def graph(value: hg.TS[Record]) -> hg.TS[int]:
        return hg.operators.getattr(value, 1)

    @hg.graph
    def explicit_output_graph(value: hg.TS[Record]) -> hg.TS[int]:
        return hg.operators.getattr[int](value, 1)

    @hg.graph
    def null_symbol_graph(value: hg.TS[Record]) -> hg.TS[int]:
        return hg.operators.getattr[int](value, hg.Symbol())

    with pytest.raises(ValueError, match="Cannot resolve operator getattr"):
        hg.run_graph(graph, [Record()])

    with pytest.raises(ValueError, match="Cannot resolve operator getattr"):
        hg.run_graph(explicit_output_graph, [Record()])

    with pytest.raises(ValueError):
        hg.run_graph(null_symbol_graph, [Record()])


def test_python_object_operator_propagates_python_exception():
    error = RuntimeError("lookup failed")

    class Indexed:
        def __getitem__(self, key: int) -> int:
            raise error

    @hg.graph
    def graph(value: hg.TS[Indexed]) -> hg.TS[int]:
        return value[0]

    with pytest.raises(RuntimeError, match="lookup failed") as raised:
        hg.run_graph(graph, [Indexed()])

    assert raised.value is error


def test_python_object_operators_require_scalar_type_arguments():
    class Indexed:
        def __getitem__(self, key: int) -> int:
            return key

    class Record:
        count: int

    @hg.graph
    def getitem_graph(value: hg.TS[Indexed]) -> hg.TS[int]:
        return hg.operators.getitem[hg.TS[int]](value, 0)

    @hg.graph
    def getattr_graph(value: hg.TS[Record]) -> hg.TS[int]:
        return hg.operators.getattr[hg.TS[int]](value, "count")

    with pytest.raises(ValueError, match="type argument does not match"):
        hg.run_graph(getitem_graph, [Indexed()])

    with pytest.raises(ValueError, match="type argument does not match"):
        hg.run_graph(getattr_graph, [Record()])


def test_getitem_keeps_existing_time_series_collection_overload():
    @hg.graph
    def graph(value: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return value[1]

    assert hg.run_graph(graph, [{0: 3, 1: 5}]) == [5]


@pytest.mark.parametrize("result_type", [int, str])
def test_getitem_rejects_explicit_type_for_time_series_collections(result_type):
    @hg.graph
    def list_graph(value: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return hg.operators.getitem[result_type](value, 1)

    @hg.graph
    def dict_graph(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.getitem[result_type](value, 1)

    with pytest.raises(ValueError, match="does not accept the explicit type argument"):
        hg.run_graph(list_graph, [{0: 3, 1: 5}])

    with pytest.raises(ValueError, match="does not accept the explicit type argument"):
        hg.run_graph(dict_graph, [{1: 9}])


@pytest.mark.parametrize("result_type", [int, str])
def test_getattr_rejects_explicit_type_for_time_series_bundle(result_type):
    @hg.schema
    class Pair:
        left: hg.TS[int]

    @hg.graph
    def graph(value: hg.TSB[Pair]) -> hg.TS[int]:
        return hg.operators.getattr[result_type](value, hg.symbol("left"))

    with pytest.raises(ValueError, match="does not accept the explicit type argument"):
        hg.run_graph(graph, [{"left": 4}])
