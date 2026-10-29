import bg


def test_flip_defaults_to_unique_values_and_accepts_named_ts():
    @bg.compute_node
    def materialize(value: bg.TSD[str, bg.TS[int]]) -> bg.TSD[str, bg.TS[int]]:
        return value.delta_value

    @bg.graph
    def flip_values(ts: bg.TSD[int, bg.TS[str]]) -> bg.TSD[str, bg.TS[int]]:
        return materialize(bg.operators.flip(ts=ts))

    assert bg.run_graph(flip_values, [{1: "one", 2: "two"}, {1: "two"}]) == [
        {"one": 1, "two": 2},
        {"one": bg.REMOVE, "two": 1},
    ]


def test_flip_can_group_duplicate_values_into_key_sets():
    @bg.graph
    def flip_values(ts: bg.TSD[int, bg.TS[str]]) -> bg.TSD[str, bg.TSS[int]]:
        return bg.operators.flip(ts=ts, unique=False)

    assert bg.run_graph(flip_values, [{1: "same", 2: "same"}, {2: bg.REMOVE}]) == [
        {"same": bg.SetDelta(added={1, 2})},
        {"same": bg.SetDelta(removed={2})},
    ]


def test_unpartition_accepts_named_ts():
    @bg.compute_node
    def materialize(value: bg.TSD[int, bg.TS[int]]) -> bg.TSD[int, bg.TS[int]]:
        return value.delta_value

    @bg.graph
    def flatten(ts: bg.TSD[str, bg.TSD[int, bg.TS[int]]]) -> bg.TSD[int, bg.TS[int]]:
        return materialize(bg.operators.unpartition(ts=ts))

    assert bg.run_graph(flatten, [{"odd": {1: 10}, "even": {2: 20}}, {"odd": {1: bg.REMOVE}}]) == [
        {1: 10, 2: 20},
        {1: bg.REMOVE},
    ]


def test_flip_keys_accepts_named_ts():
    @bg.compute_node
    def materialize(value: bg.TSD[str, bg.TSD[int, bg.TS[int]]]) -> bg.TSD[str, bg.TSD[int, bg.TS[int]]]:
        return value.delta_value

    @bg.graph
    def transpose(ts: bg.TSD[int, bg.TSD[str, bg.TS[int]]]) -> bg.TSD[str, bg.TSD[int, bg.TS[int]]]:
        return materialize(bg.operators.flip_keys(ts=ts))

    assert bg.run_graph(transpose, [{1: {"a": 5, "b": 6}, 2: {"b": 7}}, {1: {"a": bg.REMOVE}}]) == [
        {"a": {1: 5}, "b": {1: 6, 2: 7}},
        {"a": bg.REMOVE},
    ]


def test_collapse_keys_accepts_named_ts():
    @bg.compute_node
    def materialize(value: bg.TSD[tuple[int, str], bg.TS[int]]) -> bg.TSD[tuple[int, str], bg.TS[int]]:
        return value.delta_value

    @bg.graph
    def collapse(ts: bg.TSD[int, bg.TSD[str, bg.TS[int]]]) -> bg.TSD[tuple[int, str], bg.TS[int]]:
        return materialize(bg.operators.collapse_keys(ts=ts))

    assert bg.run_graph(collapse, [{1: {"a": 5, "b": 6}}, {1: {"a": bg.REMOVE}}]) == [
        {(1, "a"): 5, (1, "b"): 6},
        {(1, "a"): bg.REMOVE},
    ]


def test_uncollapse_keys_accepts_named_ts():
    @bg.compute_node
    def materialize(value: bg.TSD[int, bg.TSD[str, bg.TS[int]]]) -> bg.TSD[int, bg.TSD[str, bg.TS[int]]]:
        return value.delta_value

    @bg.graph
    def uncollapse(ts: bg.TSD[tuple[int, str], bg.TS[int]]) -> bg.TSD[int, bg.TSD[str, bg.TS[int]]]:
        return materialize(bg.operators.uncollapse_keys(ts=ts))

    assert bg.run_graph(uncollapse, [{(1, "a"): 5, (1, "b"): 6}, {(1, "a"): bg.REMOVE}]) == [
        {1: {"a": 5, "b": 6}},
        {1: {"a": bg.REMOVE}},
    ]


def test_rekey_delta_includes_added_child_value_with_its_older_tick_time():
    @bg.compute_node
    def materialize(value: bg.TSD[str, bg.TS[int]]) -> bg.TSD[str, bg.TS[int]]:
        return value.delta_value

    @bg.graph
    def rekeyed(
        values: bg.TSD[int, bg.TS[int]], keys: bg.TSD[int, bg.TS[str]]
    ) -> bg.TSD[str, bg.TS[int]]:
        return materialize(bg.operators.rekey(ts=values, new_keys=keys))

    assert bg.run_graph(
        rekeyed,
        [{1: 10}, {1: 11}, None],
        [{1: "first"}, None, {1: "second"}],
    ) == [
        {"first": 10},
        {"first": 11},
        {"second": 11, "first": bg.REMOVE},
    ]


def test_partition_delta_includes_nested_values_for_added_groups():
    @bg.compute_node
    def materialize(value: bg.TSD[str, bg.TSD[int, bg.TS[int]]]) -> bg.TSD[str, bg.TSD[int, bg.TS[int]]]:
        return value.delta_value

    @bg.graph
    def partitioned(
        values: bg.TSD[int, bg.TS[int]], partitions: bg.TSD[int, bg.TS[str]]
    ) -> bg.TSD[str, bg.TSD[int, bg.TS[int]]]:
        return materialize(bg.operators.partition(ts=values, partitions=partitions))

    assert bg.run_graph(
        partitioned,
        [{1: 10}, {1: 11}, None],
        [{1: "odd"}, None, {1: "even"}],
    ) == [
        {"odd": {1: 10}},
        {"odd": {1: 11}},
        {"odd": bg.REMOVE, "even": {1: 11}},
    ]
