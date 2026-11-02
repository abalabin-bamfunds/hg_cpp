from typing import FrozenSet, Set, TypeVar

import pytest

import hgraph as hg


def test_frozenset_annotations_accept_builtin_and_typing_forms():
    @hg.graph
    def identity(value: hg.TS[FrozenSet[int]]) -> hg.TS[frozenset[int]]:
        return value

    assert hg.run_graph(identity, [{1, 2}, frozenset({2, 3})]) == [frozenset({1, 2}), frozenset({2, 3})]


def test_frozenset_scalar_values_are_immutable_and_accept_mutable_inputs():
    @hg.source_node
    def source() -> hg.TS[frozenset[int]]:
        return {1, 2}

    @hg.node
    def inspect(value: frozenset[int]) -> int:
        assert type(value) is frozenset
        assert value == frozenset({1, 2})
        with pytest.raises(AttributeError):
            value.add(3)
        return sum(value)

    @hg.graph
    def graph() -> hg.TS[int]:
        return inspect(source())

    assert hg.run_graph(graph) == [3]


def test_frozenset_roundtrip_supports_empty_values_and_lifecycle_transitions():
    @hg.graph
    def identity(value: hg.TS[frozenset[int]]) -> hg.TS[frozenset[int]]:
        return value

    assert hg.run_graph(identity, [set(), {1}, hg.INVALID, frozenset(), {2}]) == [
        frozenset(), frozenset({1}), None, frozenset(), frozenset({2})
    ]


def test_frozenset_annotation_resolves_generic_item_types():
    ITEM = TypeVar("ITEM")

    @hg.compute_node
    def identity(value: hg.TS[frozenset[ITEM]]) -> hg.TS[frozenset[ITEM]]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[frozenset[int]]) -> hg.TS[frozenset[int]]:
        return identity(value)

    assert hg.run_graph(graph, [{1, 2}, {2, 3}]) == [frozenset({1, 2}), frozenset({2, 3})]


def test_frozenset_conversion_recurses_through_tuple_and_dictionary_values():
    @hg.source_node
    def source() -> hg.TS[dict[frozenset[int], tuple[frozenset[str], ...]]]:
        return {frozenset({1, 2}): [{"a", "b"}, set()]}

    @hg.node
    def inspect(value: dict[frozenset[int], tuple[frozenset[str], ...]]) -> int:
        assert all(type(key) is frozenset for key in value)
        assert value[frozenset({1, 2})] == (frozenset({"a", "b"}), frozenset())
        assert all(type(item) is frozenset for item in value[frozenset({1, 2})])
        return sum(map(len, value[frozenset({1, 2})]))

    @hg.graph
    def graph() -> hg.TS[int]:
        return inspect(source())

    assert hg.run_graph(graph) == [2]


def test_frozenset_scalar_values_work_as_dictionary_keys_and_tss_items():
    @hg.compute_node
    def lookup(value: hg.TS[dict[frozenset[int], str]]) -> hg.TS[str]:
        return value.value[frozenset({1, 2})]

    @hg.graph
    def dictionary(value: hg.TS[dict[frozenset[int], str]]) -> hg.TS[str]:
        return lookup(value)

    assert hg.run_graph(dictionary, [{frozenset({1, 2}): "found"}]) == ["found"]

    @hg.compute_node
    def total(values: hg.TSS[frozenset[int]]) -> hg.TS[int]:
        return sum(len(item) for item in values.value)

    @hg.graph
    def set_items(values: hg.TSS[frozenset[int]]) -> hg.TS[int]:
        return total(values)

    assert hg.run_graph(set_items, [frozenset({frozenset({1, 2}), frozenset({3})})]) == [3]


def test_native_set_length_and_contains_operators_accept_frozenset_scalars():
    @hg.graph
    def size(values: hg.TS[frozenset[int]]) -> hg.TS[int]:
        return hg.operators.len_(values)

    @hg.graph
    def contains(values: hg.TS[frozenset[int]]) -> hg.TS[bool]:
        return hg.operators.contains_(values, 2)

    assert hg.run_graph(size, [{1, 2}, set()]) == [2, 0]
    assert hg.run_graph(contains, [{1, 2}, {3}]) == [True, False]


def test_convert_between_frozenset_scalar_and_tss():
    @hg.graph
    def scalar_to_tss(value: hg.TS[frozenset[int]]) -> hg.TS[frozenset[int]]:
        values = hg.operators.convert[hg.TSS[int]](value)
        return hg.operators.convert[hg.TS[frozenset[int]]](values)

    @hg.graph
    def tss_to_scalar(value: hg.TSS[int]) -> hg.TS[frozenset[int]]:
        return hg.operators.convert[hg.TS[frozenset[int]]](value)

    assert hg.run_graph(scalar_to_tss, [{1, 2}, set(), {2, 3}]) == [
        frozenset({1, 2}), frozenset(), frozenset({2, 3})
    ]
    assert hg.run_graph(tss_to_scalar, [{1, 2}, {3}, hg.SetDelta(removed={1, 2, 3})]) == [
        frozenset({1, 2}), frozenset({1, 2, 3}), frozenset()
    ]


def test_nested_frozensets_and_bundle_fields_roundtrip():
    @hg.schema
    class Record:
        groups: frozenset[frozenset[int]]
        lookup: dict[frozenset[str], tuple[frozenset[str], ...]]

    record = Record(
        {frozenset({1, 2}), frozenset({3})},
        {frozenset({"key"}): (frozenset({"value"}), frozenset())},
    )

    @hg.source_node
    def source() -> hg.TS[hg.Bundle[Record]]:
        return record

    @hg.compute_node
    def inspect(value: hg.TS[hg.Bundle[Record]]) -> hg.TS[int]:
        observed = value.value
        assert type(observed.groups) is frozenset
        assert type(next(iter(observed.groups))) is frozenset
        assert observed.lookup[frozenset({"key"})] == (frozenset({"value"}), frozenset())
        return sum(len(group) for group in observed.groups)

    @hg.graph
    def graph() -> hg.TS[int]:
        return inspect(source())

    assert hg.run_graph(graph) == [3]


def test_frozenset_values_can_contain_python_object_scalars():
    class Value:
        def __init__(self, number: int):
            self.number = number

    first = Value(1)
    second = Value(2)

    @hg.node
    def total(values: frozenset[Value]) -> int:
        assert any(value is first for value in values)
        assert any(value is second for value in values)
        return sum(value.number for value in values)

    @hg.graph
    def graph() -> hg.TS[int]:
        return total({first, second})

    assert hg.run_graph(graph) == [3]


@pytest.mark.parametrize("annotation", [set, Set, set[int], Set[int]])
def test_mutable_set_annotations_are_rejected(annotation):
    with pytest.raises(TypeError, match="set annotations are mutable"):
        hg._native_type(annotation)


@pytest.mark.parametrize("annotation", [frozenset, FrozenSet])
def test_unparameterized_frozenset_annotations_are_rejected(annotation):
    with pytest.raises(TypeError, match="frozenset annotations must specify an item type"):
        hg._native_type(annotation)


def test_frozenset_annotations_reject_optional_and_time_series_items():
    with pytest.raises(TypeError, match=r"Optional\[...\] cannot be used as a set item type"):
        hg._native_type(frozenset[int | None])
    with pytest.raises(TypeError, match=r"frozenset\[T\] requires a scalar item annotation"):
        hg._native_type(frozenset[hg.TS[int]])


def test_frozenset_conversion_rejects_wrong_item_values():
    @hg.source_node
    def source() -> hg.TS[frozenset[int]]:
        return {"not an integer"}

    @hg.graph
    def graph() -> hg.TS[frozenset[int]]:
        return source()

    with pytest.raises(TypeError):
        hg.run_graph(graph)


def test_frozenset_conversion_rejects_wrong_container_values():
    @hg.source_node
    def source() -> hg.TS[frozenset[int]]:
        return [1, 2]

    @hg.graph
    def graph() -> hg.TS[frozenset[int]]:
        return source()

    with pytest.raises(TypeError, match="expected a set or frozenset"):
        hg.run_graph(graph)


def test_frozenset_conversion_propagates_item_hash_failures():
    class Value:
        fail_hash = False

        def __hash__(self):
            if self.fail_hash:
                raise TypeError("hash failed")
            return 1

    value = Value()
    values = {value}
    value.fail_hash = True

    @hg.node
    def count(items: frozenset[Value]) -> int:
        return len(items)

    @hg.graph
    def graph() -> hg.TS[int]:
        return count(values)

    with pytest.raises(TypeError, match="hash failed"):
        hg.run_graph(graph)


def test_frozenset_conversion_snapshots_mutable_values_at_wiring_time():
    @hg.node
    def count(value: frozenset[int]) -> int:
        return len(value)

    @hg.graph
    def graph() -> hg.TS[int]:
        mutable = {1, 2}
        result = count(mutable)
        mutable.add(3)
        return result

    assert hg.run_graph(graph) == [2]


def test_nested_owning_set_items_survive_wiring_scalar_conversion():
    @hg.node
    def total(values: frozenset[tuple[str, ...]]) -> int:
        return sum(len(item) for item in values)

    @hg.graph
    def graph() -> hg.TS[int]:
        return total(frozenset({("alpha", "beta"), ("gamma",)}))

    assert hg.run_graph(graph) == [3]
