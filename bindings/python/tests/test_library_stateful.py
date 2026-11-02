import gc
import weakref

import hg
import pytest


def test_count_counts_valid_updates_and_resets_after_invalidation():
    @hg.graph
    def count(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.count(value)

    @hg.graph
    def constant_count() -> hg.TS[int]:
        return hg.operators.count(hg.operators.const_(7))

    assert hg.run_graph(count, [hg.INVALID, None, 7, 7, hg.INVALID, None, 9]) == [
        None,
        None,
        1,
        2,
        None,
        None,
        1,
    ]
    assert hg.run_graph(constant_count) == [1]


def test_lag_delays_values_invalidations_and_revalidations():
    @hg.graph
    def lag(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.lag(value)

    @hg.graph
    def valid_lag(value: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.lag(value))

    assert hg.run_graph(lag, [None, 10, 20, hg.INVALID, None, 30, hg.INVALID, 40]) == [
        None,
        None,
        10,
        20,
        None,
        None,
        30,
        None,
    ]
    assert hg.run_graph(valid_lag, [10, 20, hg.INVALID, 30, 40]) == [False, True, None, False, True]


def test_lag_accepts_event_count_and_zero_is_identity():
    @hg.graph
    def lag_two(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.lag(value, 2)

    @hg.graph
    def lag_zero(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.lag(value, 0)

    @hg.graph
    def lag_negative(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.lag(value, -1)

    @hg.graph
    def valid_lag_two(value: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.lag(value, 2))

    assert hg.run_graph(lag_two, [None, 10, 20, hg.INVALID, None, 30]) == [
        None,
        None,
        None,
        10,
        None,
        20,
    ]
    assert hg.run_graph(lag_zero, [None, 10, hg.INVALID, 20]) == [None, 10, None, 20]
    assert hg.run_graph(valid_lag_two, [10, 20, hg.INVALID, 30, 40]) == [
        False,
        None,
        True,
        None,
        False,
    ]
    with pytest.raises(ValueError):
        hg.run_graph(lag_negative, [1])


def test_lag_owns_collection_scalar_snapshots():
    @hg.graph
    def lag(value: hg.TS[dict[int, str]]) -> hg.TS[dict[int, str]]:
        return hg.operators.lag(value)

    assert hg.run_graph(lag, [{1: "old"}, {1: "new"}, {2: "last"}]) == [
        None,
        {1: "old"},
        {1: "new"},
    ]


def test_lag_releases_final_python_object_snapshot_after_graph_teardown():
    class Value:
        pass

    @hg.graph
    def lag(value: hg.TS[Value]) -> hg.TS[Value]:
        return hg.operators.lag(value)

    def run():
        first = Value()
        final = Value()
        first_reference = weakref.ref(first)
        final_reference = weakref.ref(final)
        result = hg.run_graph(lag, [first, final])

        assert result[0] is None
        assert result[1] is first
        return first_reference, final_reference, result

    first_reference, final_reference, result = run()
    gc.collect()

    assert first_reference() is result[1]
    assert final_reference() is None


def test_multi_event_lag_releases_unread_owned_values_after_graph_teardown():
    class Value:
        pass

    @hg.graph
    def lag(value: hg.TS[Value]) -> hg.TS[Value]:
        return hg.operators.lag(value, 2)

    def run():
        first = Value()
        middle = Value()
        final = Value()
        first_reference = weakref.ref(first)
        middle_reference = weakref.ref(middle)
        final_reference = weakref.ref(final)
        result = hg.run_graph(lag, [first, middle, final])

        assert result == [None, None, first]
        return first_reference, middle_reference, final_reference, result

    first_reference, middle_reference, final_reference, result = run()
    gc.collect()

    assert first_reference() is result[2]
    assert middle_reference() is None
    assert final_reference() is None


def test_lag_follows_reference_retargeting():
    @hg.source_node
    def first() -> hg.TS[int]:
        return 10

    @hg.source_node
    def second() -> hg.TS[int]:
        return 20

    @hg.compute_node
    def select(use_second: hg.TS[bool], lhs: hg.REF[hg.TS[int]], rhs: hg.REF[hg.TS[int]]) -> hg.REF[hg.TS[int]]:
        return (rhs if use_second.value else lhs).value

    @hg.graph
    def lag_selected(use_second: hg.TS[bool]) -> hg.TS[int]:
        return hg.operators.lag(select(use_second, first(), second()))

    assert hg.run_graph(lag_selected, [False, True, False]) == [None, 10, 20]


def test_lag_state_is_independent_across_graph_calls():
    @hg.graph
    def delayed(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.lag(value)

    @hg.graph
    def combine(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TS[int]:
        return delayed(lhs) + delayed(rhs)

    assert hg.run_graph(combine, [1, 2, 3], [10, 20, 30]) == [None, 11, 22]
