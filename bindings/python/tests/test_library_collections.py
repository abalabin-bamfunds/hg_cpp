import bg


def test_scalar_dictionary_keys_reconcile_and_values_preserve_duplicates():
    @bg.graph
    def keys(value: bg.TS[dict[int, int]]) -> bg.TSS[int]:
        return bg.operators.keys_(value)

    @bg.graph
    def value_count(value: bg.TS[dict[int, int]]) -> bg.TS[int]:
        return bg.operators.len_(bg.operators.values_(value))

    @bg.compute_node
    def sum_values(values: bg.TS[tuple[int, ...]]) -> bg.TS[int]:
        return sum(values.value)

    @bg.graph
    def value_sum(value: bg.TS[dict[int, int]]) -> bg.TS[int]:
        return sum_values(bg.operators.values_(value))

    updates = [{1: 7, 2: 7}, {2: 7, 3: 7}, {}]
    assert bg.run_graph(keys, updates) == [
        bg.SetDelta(added={1, 2}),
        bg.SetDelta(added={3}, removed={1}),
        bg.SetDelta(removed={2, 3}),
    ]
    assert bg.run_graph(value_count, updates) == [2, 2, 0]
    assert bg.run_graph(value_sum, updates) == [14, 14, 0]


def test_tsd_values_snapshot_valid_items_and_distinct_values():
    @bg.graph
    def value_count(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return bg.operators.len_(bg.operators.values_(values))

    @bg.graph
    def distinct_values(values: bg.TSD[int, bg.TS[int]]) -> bg.TSS[int]:
        return bg.operators.values_[bg.TSS[int]](values)

    @bg.compute_node
    def sum_values(values: bg.TS[tuple[int, ...]]) -> bg.TS[int]:
        return sum(values.value)

    @bg.graph
    def value_sum(values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return sum_values(bg.operators.values_(values))

    updates = [
        {1: 10, 2: 10, 3: 20},
        {1: 20, 3: 10},
        {2: 20},
        {3: bg.REMOVE},
        {1: bg.REMOVE, 2: bg.REMOVE},
    ]
    assert bg.run_graph(value_count, updates) == [3, 3, 3, 2, 0]
    assert bg.run_graph(value_sum, updates) == [40, 40, 50, 40, 0]
    assert bg.run_graph(distinct_values, updates) == [
        bg.SetDelta(added={10, 20}),
        None,
        None,
        bg.SetDelta(removed={10}),
        bg.SetDelta(removed={20}),
    ]


def test_tsd_values_handle_item_and_root_validity_transitions():
    @bg.graph
    def value_count(selected: bg.TS[bool], values: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        current = bg.operators.if_then_else(
            selected, values, bg.operators.nothing[bg.TSD[int, bg.TS[int]]]()
        )
        return bg.operators.len_(bg.operators.values_(current))

    @bg.graph
    def distinct_values(selected: bg.TS[bool], values: bg.TSD[int, bg.TS[int]]) -> bg.TSS[int]:
        current = bg.operators.if_then_else(
            selected, values, bg.operators.nothing[bg.TSD[int, bg.TS[int]]]()
        )
        return bg.operators.values_[bg.TSS[int]](current)

    selected = [True, True, True, True, True, False, True]
    updates = [
        {1: 10, 2: 10},
        {1: bg.INVALID},
        {2: bg.INVALID},
        {1: 10},
        {1: bg.REMOVE},
        None,
        None,
    ]
    assert bg.run_graph(value_count, selected, updates) == [2, 1, 0, 1, 0, None, 0]
    assert bg.run_graph(distinct_values, selected, updates) == [
        bg.SetDelta(added={10}),
        None,
        bg.SetDelta(removed={10}),
        bg.SetDelta(added={10}),
        bg.SetDelta(removed={10}),
        None,
        bg.SetDelta(),
    ]


def test_index_of_accepts_scalar_and_time_series_needles_and_skips_invalid_tsl_items():
    @bg.graph
    def list_scalar(values: bg.TS[tuple[int, ...]]) -> bg.TS[int]:
        return bg.operators.index_of(values, 2)

    @bg.graph
    def list_series(values: bg.TS[tuple[int, ...]], item: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.index_of(values, item)

    @bg.graph
    def tsl_scalar(values: bg.TSL[bg.TS[int], 3]) -> bg.TS[int]:
        return bg.operators.index_of(values, 20)

    @bg.graph
    def tsl_series(values: bg.TSL[bg.TS[int], 3], item: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.index_of(values, item)

    lists = [(1, 2, 2), (3, 2), ()]
    tsl = [
        {0: 10, 1: bg.INVALID, 2: 20},
        {0: 20, 2: 20},
        {0: bg.INVALID, 1: 10, 2: bg.INVALID},
    ]
    assert bg.run_graph(list_scalar, lists) == [1, 1, -1]
    assert bg.run_graph(list_series, lists, [2, 3, 1]) == [1, 0, -1]
    assert bg.run_graph(tsl_scalar, tsl) == [2, 0, -1]
    assert bg.run_graph(tsl_series, tsl, [20, 10, 1]) == [2, -1, -1]
