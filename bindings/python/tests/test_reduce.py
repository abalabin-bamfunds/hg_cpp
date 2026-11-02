import operator
from typing import TypeVar

import pytest

import hg


def test_reduce_combines_dictionary_values_and_tracks_sparse_updates():
    @hg.graph
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(add, values)

    assert hg.run_graph(graph, [{1: 1, 2: 4}, {1: 2}, {1: hg.REMOVE}, {1: 7}]) == [5, 6, 4, 11]


def test_reduce_accepts_keyword_arguments_and_is_exported_from_operators():
    @hg.graph
    def add(left: int, right: int) -> int:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.reduce(function=add, values=values)

    assert hg.reduce is hg.operators.reduce
    assert hg.run_graph(graph, [{1: 2, 2: 5}]) == [7]


def test_reduce_accepts_lambdas_and_builtin_binary_callables():
    @hg.graph
    def product(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(lambda left, right: left * right, values, default=1)

    @hg.graph
    def total(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(operator.add, values)

    assert hg.run_graph(product, [{}, {1: 2, 2: 3}, {1: 4}]) == [1, 6, 12]
    assert hg.run_graph(total, [{1: 2, 2: 5}]) == [7]


def test_reduce_callable_can_capture_values():
    mask = 8

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(lambda left, right: left | right | mask, values)

    assert hg.run_graph(graph, [{1: 1, 2: 2}]) == [11]


def test_reduce_accepts_a_callable_instance_without_weak_references():
    class UnionWithMask:
        __slots__ = ("mask",)

        def __init__(self, mask: int):
            self.mask = mask

        def __call__(self, left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
            return left | right | self.mask

    combine = UnionWithMask(8)

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(combine, values)

    assert hg.run_graph(graph, [{1: 1, 2: 2}]) == [11]


def test_reduce_specializes_a_generic_time_series_reducer():
    VALUE = TypeVar("VALUE")

    @hg.graph
    def add(left: hg.TS[VALUE], right: hg.TS[VALUE]) -> hg.TS[VALUE]:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(add, values)

    assert hg.run_graph(graph, [{1: 3, 2: 4}]) == [7]


def test_reduce_uses_the_default_for_valid_empty_or_all_invalid_values():
    @hg.graph
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(add, values=values, default=20)

    assert hg.run_graph(graph, [{}, hg.INVALID, {1: hg.INVALID}, {1: 4}]) == [20, None, 20, 4]


def test_reduce_without_a_default_has_no_result_for_empty_values():
    @hg.graph
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(add, values)

    assert hg.run_graph(graph, [{}, {1: 1}, {1: hg.REMOVE}]) == [None, 1, None]


def test_reduce_rejects_none_as_an_explicit_default():
    @hg.graph
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(add, values, None)

    with pytest.raises(ValueError, match="reduce default cannot be None"):
        hg.run_graph(graph, [{}])


def test_reduce_retains_a_scalar_collection_default_after_wiring():
    @hg.graph
    def first(left: hg.TS[tuple[int, ...]], right: hg.TS[tuple[int, ...]]) -> hg.TS[tuple[int, ...]]:
        return left

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[tuple[int, ...]]]) -> hg.TS[tuple[int, ...]]:
        return hg.reduce(first, values, default=(8, 9))

    assert hg.run_graph(graph, [{}]) == [(8, 9)]


@pytest.mark.parametrize("use_callable", [False, True])
def test_reduce_accepts_a_time_series_default_and_tracks_reference_updates(use_callable):
    @hg.compute_node
    def values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TSS[int]]:
        if trigger.value == 1:
            return {1: {1, 2}, 2: {2, 3}}
        if trigger.value == 2:
            return {1: hg.REMOVE, 2: hg.REMOVE}
        return None

    @hg.compute_node
    def fallback(trigger: hg.TS[int]) -> hg.TSS[int]:
        if trigger.value <= 2:
            return {8}
        if trigger.value == 3:
            return {8, 9}
        return {8, 9, 10}

    @hg.graph
    def union(left: hg.TSS[int], right: hg.TSS[int]) -> hg.TSS[int]:
        return left | right

    reducer = (lambda left, right: left | right) if use_callable else union

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.len_(hg.reduce(reducer, values(trigger), default=fallback(trigger)))

    assert hg.run_graph(graph, [1, 2, 3, 4]) == [3, 1, 2, 3]


@pytest.mark.parametrize("use_callable", [False, True])
def test_reduce_observes_updates_through_reference_backed_items(use_callable):
    @hg.compute_node
    def make_references(
        trigger: hg.TS[int], left: hg.REF[hg.TS[int]], right: hg.REF[hg.TS[int]]
    ) -> hg.TSD[int, hg.REF[hg.TS[int]]]:
        if trigger.value == 1:
            return {1: left.value, 2: right.value}
        return None

    @hg.graph
    def add(left: int, right: int) -> int:
        return left + right

    reducer = (lambda lhs, rhs: lhs + rhs) if use_callable else add

    @hg.graph
    def graph(trigger: hg.TS[int], left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return hg.reduce(reducer, make_references(trigger, left, right))

    assert hg.run_graph(graph, [1, 2, 3], [10, 20, 30], [1, 2, 3]) == [11, 22, 33]


@pytest.mark.parametrize("use_callable", [False, True])
def test_reduce_weakens_final_item_guarantees_for_tree_reuse(use_callable):
    @hg.compute_node
    def values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int, hg.final]]:
        if trigger.value == 1:
            return {1: 10}
        if trigger.value == 2:
            return {1: 10, 2: 20}
        return {1: 10, 2: 20, 3: 30}

    @hg.graph
    def add(left: int, right: int) -> int:
        return left + right

    reducer = (lambda left, right: left + right) if use_callable else add

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.reduce(reducer, values(trigger))

    assert hg.run_graph(graph, [1, 2, 3]) == [10, 30, 60]


def test_reduce_preserves_python_exceptions_from_reducer_wiring():
    class ReducerError(Exception):
        pass

    error = ReducerError("reducer wiring failed")

    @hg.graph
    def fail(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        raise error

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(fail, values)

    with pytest.raises(ReducerError, match="reducer wiring failed") as caught:
        hg.run_graph(graph, [{1: 2, 2: 3}])
    assert caught.value is error


def test_reduce_rejects_reducer_with_the_wrong_number_of_inputs():
    @hg.graph
    def identity(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(identity, values)

    with pytest.raises(ValueError, match="exactly two explicit inputs"):
        hg.run_graph(graph, [{1: 2, 2: 3}])


def test_reduce_rejects_a_sink_reducer():
    @hg.graph
    def sink(left: hg.TS[int], right: hg.TS[int]) -> None:
        pass

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(sink, values)

    with pytest.raises(ValueError, match="output must be a time series"):
        hg.run_graph(graph, [{1: 2, 2: 3}])


def test_reduce_rejects_incompatible_reducer_input_types():
    @hg.graph
    def concatenate(left: hg.TS[str], right: hg.TS[str]) -> hg.TS[str]:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(concatenate, values)

    with pytest.raises(ValueError, match="expected TS<string, left>"):
        hg.run_graph(graph, [{1: 2, 2: 3}])


def test_reduce_rejects_an_incompatible_reducer_output_type():
    @hg.graph
    def concatenate(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[str]:
        return hg.operators.str_(left + right)

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(concatenate, values)

    with pytest.raises(ValueError, match="output must have the TSD item value type"):
        hg.run_graph(graph, [{1: 2, 2: 3}])


def test_reduce_rejects_a_mismatched_scalar_default():
    @hg.graph
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(add, values, default="not an integer")

    with pytest.raises(TypeError, match="expected an integer for hg.i64"):
        hg.run_graph(graph, [{}])


def test_reduce_rejects_a_time_series_default_for_scalar_items():
    @hg.graph
    def add(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return left + right

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]], fallback: hg.TS[int]) -> hg.TS[int]:
        return hg.reduce(add, values, default=fallback)

    with pytest.raises(ValueError, match="default must be a scalar value matching the TSD item type"):
        hg.run_graph(graph, [{}], [7])


def test_reduce_callable_must_accept_two_inputs():
    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(lambda value: value, values)

    with pytest.raises(TypeError, match="takes 1 positional argument but 2 were given"):
        hg.run_graph(graph, [{1: 2, 2: 3}])


def test_reduce_callable_must_return_a_wiring_point():
    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(lambda left, right: 17, values)

    with pytest.raises(ValueError, match="must return a wiring point"):
        hg.run_graph(graph, [{1: 2, 2: 3}])


def test_reduce_callable_must_return_the_item_type():
    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(lambda left, right: hg.operators.str_(left + right), values)

    with pytest.raises(ValueError, match="expected TS<i64>, received TS<string>"):
        hg.run_graph(graph, [{1: 2, 2: 3}])


def test_reduce_preserves_exceptions_from_an_ordinary_callable():
    class ReducerError(Exception):
        pass

    error = ReducerError("callable reducer failed")

    def fail(left, right):
        raise error

    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(fail, values)

    with pytest.raises(ReducerError, match="callable reducer failed") as caught:
        hg.run_graph(graph, [{1: 2, 2: 3}])
    assert caught.value is error


def test_reduce_rejects_a_non_callable_reducer():
    @hg.graph
    def graph(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.reduce(42, values)

    with pytest.raises(TypeError, match="decorated @graph definition or callable"):
        hg.run_graph(graph, [{1: 2, 2: 3}])
