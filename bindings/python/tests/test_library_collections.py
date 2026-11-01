import hg


def test_scalar_dictionary_keys_reconcile_and_values_preserve_duplicates():
    @hg.graph
    def keys(value: hg.TS[dict[int, int]]) -> hg.TSS[int]:
        return hg.operators.keys_(value)

    @hg.graph
    def value_count(value: hg.TS[dict[int, int]]) -> hg.TS[int]:
        return hg.operators.len_(hg.operators.values_(value))

    @hg.compute_node
    def sum_values(values: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return sum(values.value)

    @hg.graph
    def value_sum(value: hg.TS[dict[int, int]]) -> hg.TS[int]:
        return sum_values(hg.operators.values_(value))

    updates = [{1: 7, 2: 7}, {2: 7, 3: 7}, {}]
    assert hg.run_graph(keys, updates) == [
        hg.SetDelta(added={1, 2}),
        hg.SetDelta(added={3}, removed={1}),
        hg.SetDelta(removed={2, 3}),
    ]
    assert hg.run_graph(value_count, updates) == [2, 2, 0]
    assert hg.run_graph(value_sum, updates) == [14, 14, 0]


def test_tsd_values_snapshot_valid_items_and_distinct_values():
    @hg.graph
    def value_count(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return hg.operators.len_(hg.operators.values_(values))

    @hg.graph
    def distinct_values(values: hg.TSD[int, hg.TS[int]]) -> hg.TSS[int]:
        return hg.operators.values_[hg.TSS[int]](values)

    @hg.compute_node
    def sum_values(values: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return sum(values.value)

    @hg.graph
    def value_sum(values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return sum_values(hg.operators.values_(values))

    updates = [
        {1: 10, 2: 10, 3: 20},
        {1: 20, 3: 10},
        {2: 20},
        {3: hg.REMOVE},
        {1: hg.REMOVE, 2: hg.REMOVE},
    ]
    assert hg.run_graph(value_count, updates) == [3, 3, 3, 2, 0]
    assert hg.run_graph(value_sum, updates) == [40, 40, 50, 40, 0]
    assert hg.run_graph(distinct_values, updates) == [
        hg.SetDelta(added={10, 20}),
        None,
        None,
        hg.SetDelta(removed={10}),
        hg.SetDelta(removed={20}),
    ]


def test_tsd_values_handle_item_and_root_validity_transitions():
    @hg.graph
    def value_count(selected: hg.TS[bool], values: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        current = hg.operators.if_then_else(
            selected, values, hg.operators.nothing[hg.TSD[int, hg.TS[int]]]()
        )
        return hg.operators.len_(hg.operators.values_(current))

    @hg.graph
    def distinct_values(selected: hg.TS[bool], values: hg.TSD[int, hg.TS[int]]) -> hg.TSS[int]:
        current = hg.operators.if_then_else(
            selected, values, hg.operators.nothing[hg.TSD[int, hg.TS[int]]]()
        )
        return hg.operators.values_[hg.TSS[int]](current)

    selected = [True, True, True, True, True, False, True]
    updates = [
        {1: 10, 2: 10},
        {1: hg.INVALID},
        {2: hg.INVALID},
        {1: 10},
        {1: hg.REMOVE},
        None,
        None,
    ]
    assert hg.run_graph(value_count, selected, updates) == [2, 1, 0, 1, 0, None, 0]
    assert hg.run_graph(distinct_values, selected, updates) == [
        hg.SetDelta(added={10}),
        None,
        hg.SetDelta(removed={10}),
        hg.SetDelta(added={10}),
        hg.SetDelta(removed={10}),
        None,
        hg.SetDelta(),
    ]


def test_index_of_accepts_scalar_and_time_series_needles_and_skips_invalid_tsl_items():
    @hg.graph
    def list_scalar(values: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return hg.operators.index_of(values, 2)

    @hg.graph
    def list_series(values: hg.TS[tuple[int, ...]], item: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.index_of(values, item)

    @hg.graph
    def tsl_scalar(values: hg.TSL[hg.TS[int], 3]) -> hg.TS[int]:
        return hg.operators.index_of(values, 20)

    @hg.graph
    def tsl_series(values: hg.TSL[hg.TS[int], 3], item: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.index_of(values, item)

    lists = [(1, 2, 2), (3, 2), ()]
    tsl = [
        {0: 10, 1: hg.INVALID, 2: 20},
        {0: 20, 2: 20},
        {0: hg.INVALID, 1: 10, 2: hg.INVALID},
    ]
    assert hg.run_graph(list_scalar, lists) == [1, 1, -1]
    assert hg.run_graph(list_series, lists, [2, 3, 1]) == [1, 0, -1]
    assert hg.run_graph(tsl_scalar, tsl) == [2, 0, -1]
    assert hg.run_graph(tsl_series, tsl, [20, 10, 1]) == [2, -1, -1]
