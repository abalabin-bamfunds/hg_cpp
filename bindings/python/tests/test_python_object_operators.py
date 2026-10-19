import pytest

import bg


def test_getitem_infers_python_object_result_for_scalar_key():
    class Indexed:
        def __getitem__(self, key: int) -> str:
            return f"item-{key}"

    value = Indexed()

    @bg.graph
    def graph(value: bg.TS[Indexed]) -> bg.TS[str]:
        return bg.operators.getitem(value, 3)

    assert bg.test_graph(graph, [value])[0] == "item-3"


def test_getitem_accepts_time_series_key_for_python_object():
    class Indexed:
        def __getitem__(self, key: int) -> str:
            return f"item-{key}"

    value = Indexed()

    @bg.graph
    def graph(value: bg.TS[Indexed], key: bg.TS[int]) -> bg.TS[str]:
        return value[key]

    assert bg.test_graph(graph, [value, value], [1, 2]) == ["item-1", "item-2"]


def test_getitem_accepts_unhashable_python_object_key():
    class Key:
        __hash__ = None

    class Indexed:
        def __getitem__(self, key: Key) -> int:
            return 7

    @bg.graph
    def graph(value: bg.TS[Indexed], key: bg.TS[Key]) -> bg.TS[int]:
        return value[key]

    assert bg.test_graph(graph, [Indexed()], [Key()]) == [7]


def test_getitem_accepts_explicit_output_type_without_annotation():
    class Indexed:
        def __getitem__(self, key):
            return key * 2

    value = Indexed()

    @bg.graph
    def graph(value: bg.TS[Indexed]) -> bg.TS[int]:
        return bg.operators.getitem[bg.TS[int]](value, 4)

    assert bg.test_graph(graph, [value]) == [8]


def test_python_object_getitem_result_supports_covariant_output_type():
    class Base:
        pass

    class Derived(Base):
        pass

    class Indexed:
        def __getitem__(self, key: int) -> Base:
            return Derived()

    @bg.graph
    def graph(value: bg.TS[Indexed]) -> bg.TS[Base]:
        return value[0]

    result = bg.test_graph(graph, [Indexed()])[0]
    assert type(result) is Derived


def test_getattr_infers_annotated_field_and_property_types():
    class Record:
        name: str

        @property
        def count(self) -> int:
            return 2

    record = Record()
    record.name = "record"

    @bg.graph
    def read_name(value: bg.TS[Record]) -> bg.TS[str]:
        return bg.operators.getattr(value, bg.symbol("name"))

    @bg.graph
    def read_count(value: bg.TS[Record]) -> bg.TS[int]:
        return bg.operators.getattr(value, bg.symbol("count"))

    assert bg.test_graph(read_name, [record]) == ["record"]
    assert bg.test_graph(read_count, [record]) == [2]


def test_getattr_accepts_explicit_output_type_and_plain_string_name():
    class Record:
        @property
        def count(self):
            return 2

    @bg.graph
    def graph(value: bg.TS[Record]) -> bg.TS[int]:
        return bg.operators.getattr[bg.TS[int]](value, "count")

    assert bg.test_graph(graph, [Record()]) == [2]


def test_getattr_rejects_non_string_attribute_name():
    class Record:
        pass

    @bg.graph
    def graph(value: bg.TS[Record]) -> bg.TS[int]:
        return bg.operators.getattr(value, 1)

    with pytest.raises(ValueError, match="Cannot resolve operator getattr"):
        bg.test_graph(graph, [Record()])


def test_python_object_operator_propagates_python_exception():
    error = RuntimeError("lookup failed")

    class Indexed:
        def __getitem__(self, key: int) -> int:
            raise error

    @bg.graph
    def graph(value: bg.TS[Indexed]) -> bg.TS[int]:
        return value[0]

    with pytest.raises(RuntimeError, match="lookup failed") as raised:
        bg.test_graph(graph, [Indexed()])

    assert raised.value is error


def test_getattr_rejects_scalar_type_as_output_type_argument():
    class Record:
        count: int

    @bg.graph
    def graph(value: bg.TS[Record]) -> bg.TS[int]:
        return bg.operators.getattr[int](value, "count")

    with pytest.raises((TypeError, ValueError)):
        bg.test_graph(graph, [Record()])


def test_getitem_keeps_existing_time_series_collection_overload():
    @bg.graph
    def graph(value: bg.TSL[bg.TS[int], 2]) -> bg.TS[int]:
        return value[1]

    assert bg.test_graph(graph, [{0: 3, 1: 5}]) == [5]
