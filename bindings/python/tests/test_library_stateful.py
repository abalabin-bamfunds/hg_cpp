import gc
import weakref

import bg


def test_count_counts_valid_updates_and_resets_after_invalidation():
    @bg.graph
    def count(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.count(value)

    @bg.graph
    def constant_count() -> bg.TS[int]:
        return bg.operators.count(bg.operators.const_(7))

    assert bg.run_graph(count, [bg.INVALID, None, 7, 7, bg.INVALID, None, 9]) == [
        None,
        None,
        1,
        2,
        None,
        None,
        1,
    ]
    assert bg.run_graph(constant_count) == [1]


def test_lag_delays_values_invalidations_and_revalidations():
    @bg.graph
    def lag(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.lag(value)

    @bg.graph
    def valid_lag(value: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.lag(value))

    assert bg.run_graph(lag, [None, 10, 20, bg.INVALID, None, 30, bg.INVALID, 40]) == [
        None,
        None,
        10,
        20,
        None,
        None,
        30,
        None,
    ]
    assert bg.run_graph(valid_lag, [10, 20, bg.INVALID, 30, 40]) == [False, True, None, False, True]


def test_lag_owns_collection_scalar_snapshots():
    @bg.graph
    def lag(value: bg.TS[dict[int, str]]) -> bg.TS[dict[int, str]]:
        return bg.operators.lag(value)

    assert bg.run_graph(lag, [{1: "old"}, {1: "new"}, {2: "last"}]) == [
        None,
        {1: "old"},
        {1: "new"},
    ]


def test_lag_releases_final_python_object_snapshot_after_graph_teardown():
    class Value:
        pass

    @bg.graph
    def lag(value: bg.TS[Value]) -> bg.TS[Value]:
        return bg.operators.lag(value)

    def run():
        first = Value()
        final = Value()
        first_reference = weakref.ref(first)
        final_reference = weakref.ref(final)
        result = bg.run_graph(lag, [first, final])

        assert result[0] is None
        assert result[1] is first
        return first_reference, final_reference, result

    first_reference, final_reference, result = run()
    gc.collect()

    assert first_reference() is result[1]
    assert final_reference() is None


def test_lag_follows_reference_retargeting():
    @bg.source_node
    def first() -> bg.TS[int]:
        return 10

    @bg.source_node
    def second() -> bg.TS[int]:
        return 20

    @bg.compute_node
    def select(use_second: bg.TS[bool], lhs: bg.REF[bg.TS[int]], rhs: bg.REF[bg.TS[int]]) -> bg.REF[bg.TS[int]]:
        return (rhs if use_second.value else lhs).value

    @bg.graph
    def lag_selected(use_second: bg.TS[bool]) -> bg.TS[int]:
        return bg.operators.lag(select(use_second, first(), second()))

    assert bg.run_graph(lag_selected, [False, True, False]) == [None, 10, 20]


def test_lag_state_is_independent_across_graph_calls():
    @bg.graph
    def delayed(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.lag(value)

    @bg.graph
    def combine(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return delayed(lhs) + delayed(rhs)

    assert bg.run_graph(combine, [1, 2, 3], [10, 20, 30]) == [None, 11, 22]
