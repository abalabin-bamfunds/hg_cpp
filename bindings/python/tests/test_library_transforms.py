import hgraph as hg


def test_flip_defaults_to_unique_values_and_accepts_named_ts():
    @hg.compute_node
    def materialize(value: hg.TSD[str, hg.TS[int]]) -> hg.TSD[str, hg.TS[int]]:
        return value.delta_value

    @hg.graph
    def flip_values(ts: hg.TSD[int, hg.TS[str]]) -> hg.TSD[str, hg.TS[int]]:
        return materialize(hg.operators.flip(ts=ts))

    assert hg.run_graph(flip_values, [{1: "one", 2: "two"}, {1: "two"}]) == [
        {"one": 1, "two": 2},
        {"one": hg.REMOVE, "two": 1},
    ]


def test_flip_can_group_duplicate_values_into_key_sets():
    @hg.graph
    def flip_values(ts: hg.TSD[int, hg.TS[str]]) -> hg.TSD[str, hg.TSS[int]]:
        return hg.operators.flip(ts=ts, unique=False)

    assert hg.run_graph(flip_values, [{1: "same", 2: "same"}, {2: hg.REMOVE}]) == [
        {"same": hg.SetDelta(added={1, 2})},
        {"same": hg.SetDelta(removed={2})},
    ]


def test_unpartition_accepts_named_ts():
    @hg.compute_node
    def materialize(value: hg.TSD[int, hg.TS[int]]) -> hg.TSD[int, hg.TS[int]]:
        return value.delta_value

    @hg.graph
    def flatten(ts: hg.TSD[str, hg.TSD[int, hg.TS[int]]]) -> hg.TSD[int, hg.TS[int]]:
        return materialize(hg.operators.unpartition(ts=ts))

    assert hg.run_graph(flatten, [{"odd": {1: 10}, "even": {2: 20}}, {"odd": {1: hg.REMOVE}}]) == [
        {1: 10, 2: 20},
        {1: hg.REMOVE},
    ]


def test_flip_keys_accepts_named_ts():
    @hg.compute_node
    def materialize(value: hg.TSD[str, hg.TSD[int, hg.TS[int]]]) -> hg.TSD[str, hg.TSD[int, hg.TS[int]]]:
        return value.delta_value

    @hg.graph
    def transpose(ts: hg.TSD[int, hg.TSD[str, hg.TS[int]]]) -> hg.TSD[str, hg.TSD[int, hg.TS[int]]]:
        return materialize(hg.operators.flip_keys(ts=ts))

    assert hg.run_graph(transpose, [{1: {"a": 5, "b": 6}, 2: {"b": 7}}, {1: {"a": hg.REMOVE}}]) == [
        {"a": {1: 5}, "b": {1: 6, 2: 7}},
        {"a": hg.REMOVE},
    ]


def test_collapse_keys_accepts_named_ts():
    @hg.compute_node
    def materialize(value: hg.TSD[tuple[int, str], hg.TS[int]]) -> hg.TSD[tuple[int, str], hg.TS[int]]:
        return value.delta_value

    @hg.graph
    def collapse(ts: hg.TSD[int, hg.TSD[str, hg.TS[int]]]) -> hg.TSD[tuple[int, str], hg.TS[int]]:
        return materialize(hg.operators.collapse_keys(ts=ts))

    assert hg.run_graph(collapse, [{1: {"a": 5, "b": 6}}, {1: {"a": hg.REMOVE}}]) == [
        {(1, "a"): 5, (1, "b"): 6},
        {(1, "a"): hg.REMOVE},
    ]


def test_uncollapse_keys_accepts_named_ts():
    @hg.compute_node
    def materialize(value: hg.TSD[int, hg.TSD[str, hg.TS[int]]]) -> hg.TSD[int, hg.TSD[str, hg.TS[int]]]:
        return value.delta_value

    @hg.graph
    def uncollapse(ts: hg.TSD[tuple[int, str], hg.TS[int]]) -> hg.TSD[int, hg.TSD[str, hg.TS[int]]]:
        return materialize(hg.operators.uncollapse_keys(ts=ts))

    assert hg.run_graph(uncollapse, [{(1, "a"): 5, (1, "b"): 6}, {(1, "a"): hg.REMOVE}]) == [
        {1: {"a": 5, "b": 6}},
        {1: {"a": hg.REMOVE}},
    ]


def test_rekey_delta_includes_added_child_value_with_its_older_tick_time():
    @hg.compute_node
    def materialize(value: hg.TSD[str, hg.TS[int]]) -> hg.TSD[str, hg.TS[int]]:
        return value.delta_value

    @hg.graph
    def rekeyed(
        values: hg.TSD[int, hg.TS[int]], keys: hg.TSD[int, hg.TS[str]]
    ) -> hg.TSD[str, hg.TS[int]]:
        return materialize(hg.operators.rekey(ts=values, new_keys=keys))

    assert hg.run_graph(
        rekeyed,
        [{1: 10}, {1: 11}, None],
        [{1: "first"}, None, {1: "second"}],
    ) == [
        {"first": 10},
        {"first": 11},
        {"second": 11, "first": hg.REMOVE},
    ]


def test_partition_delta_includes_nested_values_for_added_groups():
    @hg.compute_node
    def materialize(value: hg.TSD[str, hg.TSD[int, hg.TS[int]]]) -> hg.TSD[str, hg.TSD[int, hg.TS[int]]]:
        return value.delta_value

    @hg.graph
    def partitioned(
        values: hg.TSD[int, hg.TS[int]], partitions: hg.TSD[int, hg.TS[str]]
    ) -> hg.TSD[str, hg.TSD[int, hg.TS[int]]]:
        return materialize(hg.operators.partition(ts=values, partitions=partitions))

    assert hg.run_graph(
        partitioned,
        [{1: 10}, {1: 11}, None],
        [{1: "odd"}, None, {1: "even"}],
    ) == [
        {"odd": {1: 10}},
        {"odd": {1: 11}},
        {"odd": hg.REMOVE, "even": {1: 11}},
    ]
