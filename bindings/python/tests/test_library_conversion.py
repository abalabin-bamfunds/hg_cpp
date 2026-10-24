import bg
import pytest


@bg.schema
class ScalarRecord:
    count: int
    ratio: float


@bg.schema
class SeriesRecord:
    ratio: bg.TS[float]
    count: bg.TS[int]


def test_convert_copies_fixed_tsl_snapshots_to_and_from_scalar_lists():
    @bg.compute_node
    def sum_list(value: bg.TS[tuple[int, ...]]) -> bg.TS[int]:
        return sum(value.value)

    @bg.graph
    def list_to_tsl(value: bg.TS[tuple[int, ...]]) -> bg.TS[int]:
        snapshot = bg.operators.convert[bg.TSL[bg.TS[int], 2]](value)
        return sum_list(bg.operators.convert[bg.TS[tuple[int, ...]]](snapshot))

    @bg.graph
    def tsl_to_list(value: bg.TSL[bg.TS[int], 2]) -> bg.TS[int]:
        return sum_list(bg.operators.convert[bg.TS[tuple[int, ...]]](value))

    assert bg.run_graph(list_to_tsl, [[1, 2], [3, 4]]) == [3, 7]
    assert bg.run_graph(tsl_to_list, [{0: 1, 1: 2}, {0: 3}]) == [3, 5]


def test_convert_copies_scalar_list_and_dictionary_collection_snapshots():
    @bg.compute_node
    def sum_tss(value: bg.TSS[int]) -> bg.TS[int]:
        return sum(value.value)

    @bg.graph
    def list_to_tss(value: bg.TS[tuple[int, ...]]) -> bg.TS[int]:
        return sum_tss(bg.operators.convert[bg.TSS[int]](value))

    @bg.compute_node
    def sum_dict(value: bg.TS[dict[int, int]]) -> bg.TS[int]:
        return sum(value.value.values())

    @bg.compute_node
    def sum_tsd(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return sum(value.value.values())

    @bg.graph
    def tsd_to_dict(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        return sum_dict(bg.operators.convert[bg.TS[dict[int, int]]](value))

    @bg.graph
    def dict_to_tsd(value: bg.TS[dict[int, int]]) -> bg.TS[int]:
        return sum_tsd(bg.operators.convert[bg.TSD[int, bg.TS[int]]](value))

    assert bg.run_graph(list_to_tss, [[1, 2], [2, 3]]) == [3, 5]
    assert bg.run_graph(tsd_to_dict, [{1: 10, 2: 20}, {2: 7}, {2: bg.REMOVE}]) == [30, 17, 10]
    assert bg.run_graph(dict_to_tsd, [{1: 10, 2: 20}, {2: 7}, {}]) == [30, 7, 0]


def test_convert_maps_runtime_bundle_and_tsb_schemas_by_field_name():
    @bg.compute_node
    def summarize(value: bg.TSB[SeriesRecord]) -> bg.TS[float]:
        return value.count.value + value.ratio.value

    @bg.graph
    def bundle_to_tsb(value: bg.TS[bg.Bundle[ScalarRecord]]) -> bg.TS[float]:
        converted = bg.operators.convert[bg.TSB[SeriesRecord]](value)
        return summarize(converted)

    @bg.compute_node
    def read_bundle(value: bg.TS[bg.Bundle[ScalarRecord]]) -> bg.TS[float]:
        return value.value.count + value.value.ratio

    @bg.graph
    def tsb_to_bundle(value: bg.TSB[SeriesRecord]) -> bg.TS[float]:
        converted = bg.operators.convert[bg.TS[bg.Bundle[ScalarRecord]]](value)
        return read_bundle(converted)

    assert bg.run_graph(bundle_to_tsb, [ScalarRecord(count=2, ratio=0.5)]) == [2.5]
    assert bg.run_graph(tsb_to_bundle, [{"ratio": 0.5, "count": 2}]) == [2.5]


def test_combine_publishes_required_and_initially_partial_unmarked_values():
    @bg.graph
    def combine_tsl(first: bg.TS[int], second: bg.TS[int]) -> bg.TSL[bg.TS[int], 2]:
        return bg.operators.combine[bg.TSL[bg.TS[int], 2]](first, second)

    @bg.graph
    def combine_named(count: bg.TS[int], ratio: bg.TS[float]) -> bg.TSB[SeriesRecord]:
        return bg.operators.combine[bg.TSB[SeriesRecord]](ratio=ratio, count=count)

    @bg.graph
    def combine_partial_tsl(first: bg.TS[int], second: bg.TS[int]) -> bg.TSL[bg.TS[int], 2]:
        return bg.operators.combine[bg.TSL[bg.TS[int], 2]](first, second)

    @bg.graph
    def combine_partial_named(count: bg.TS[int], ratio: bg.TS[float]) -> bg.TSB[SeriesRecord]:
        return bg.operators.combine[bg.TSB[SeriesRecord]](ratio=ratio, count=count)

    @bg.graph
    def combine_optional(count: bg.TS[int], ratio: bg.TS[float]) -> bg.TSB[SeriesRecord]:
        return bg.operators.combine[bg.TSB[SeriesRecord]](count=bg.opt(count), ratio=bg.opt(ratio))

    assert bg.run_graph(combine_tsl, [1, 2], [10, None]) == [{0: 1, 1: 10}, {0: 2}]
    assert bg.run_graph(combine_named, [7, 8], [2.5, None]) == [
        {"count": 7, "ratio": 2.5},
        {"count": 8},
    ]
    assert bg.run_graph(combine_partial_tsl, [None, 2], [10, None]) == [{1: 10}, {0: 2}]
    assert bg.run_graph(combine_partial_named, [None, 7], [2.5, None]) == [
        {"ratio": 2.5},
        {"count": 7},
    ]
    assert bg.run_graph(combine_optional, [bg.INVALID, 7, bg.INVALID, 9], [2.5, None, bg.INVALID, 3.5]) == [
        {"ratio": 2.5},
        {"count": 7},
        None,
        {"count": 9, "ratio": 3.5},
    ]


def test_collect_accumulates_collection_snapshots_and_preserves_history_on_invalidation():
    @bg.graph
    def collect_list(value: bg.TS[int]) -> bg.TS[tuple[int, ...]]:
        return bg.operators.collect[bg.TS[tuple[int, ...]]](value)

    @bg.graph
    def collect_dict(key: bg.TS[int], value: bg.TS[int]) -> bg.TS[dict[int, int]]:
        return bg.operators.collect[bg.TS[dict[int, int]]](key=key, value=value)

    assert bg.run_graph(collect_list, [1, 1, bg.INVALID, 2]) == [(1,), (1, 1), None, (1, 1, 2)]
    assert bg.run_graph(collect_dict, [1, 1, 2], [10, 11, bg.INVALID]) == [
        {1: 10},
        {1: 11},
        None,
    ]

    @bg.graph
    def collect_list_with_reset(value: bg.TS[int], reset: bg.TS[bool]) -> bg.TS[tuple[int, ...]]:
        return bg.operators.collect[bg.TS[tuple[int, ...]]](value, reset=reset)

    assert bg.run_graph(collect_list_with_reset, [1, 2, 3, 4], [None, None, False, None]) == [
        (1,),
        (1, 2),
        (3,),
        (3, 4),
    ]

    @bg.graph
    def collect_set_with_reset(value: bg.TS[int], reset: bg.TS[bool]) -> bg.TSS[int]:
        return bg.operators.collect[bg.TSS[int]](value, reset=reset)

    assert bg.run_graph(collect_set_with_reset, [1, 2, 3, 4], [None, None, False, None]) == [
        bg.SetDelta(added={1}),
        bg.SetDelta(added={2}),
        bg.SetDelta(added={3}, removed={1, 2}),
        bg.SetDelta(added={4}),
    ]

    @bg.graph
    def collect_tsd_with_reset(key: bg.TS[int], value: bg.TS[int], reset: bg.TS[bool]) -> bg.TSD[int, bg.TS[int]]:
        return bg.operators.collect[bg.TSD[int, bg.TS[int]]](key=key, value=value, reset=reset)

    assert bg.run_graph(collect_tsd_with_reset, [1, 2], [10, 20], [None, False]) == [
        {1: 10},
        {1: bg.REMOVE, 2: 20},
    ]


def test_emit_serializes_scalar_collections_and_dictionary_entries():
    @bg.graph
    def emit_list(value: bg.TS[tuple[int, ...]]) -> bg.TS[int]:
        return bg.operators.emit(value)

    @bg.graph
    def emit_dictionary(value: bg.TS[dict[int, int]]) -> bg.TS[int]:
        entry = bg.operators.emit(value)
        key = bg.operators.getattr(entry, bg.symbol("key"))
        item = bg.operators.getattr(entry, bg.symbol("value"))
        return key + item

    @bg.graph
    def emit_tsd(value: bg.TSD[int, bg.TS[int]]) -> bg.TS[int]:
        entry = bg.operators.emit(value)
        key = bg.operators.getattr(entry, bg.symbol("key"))
        item = bg.operators.getattr(entry, bg.symbol("value"))
        return key + item

    @bg.graph
    def emit_tss(value: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.emit(value)

    @bg.graph
    def emit_tsl(value: bg.TSL[bg.TS[int], 2]) -> bg.TS[int]:
        return bg.operators.emit(value)

    assert bg.run_graph(emit_list, [[1, 2], [], [3], bg.INVALID]) == [1, 2, 3, None]
    assert bg.run_graph(emit_dictionary, [{1: 10}, {2: 20}]) == [11, 22]
    assert sorted(bg.run_graph(emit_tss, [{1, 2}, bg.SetDelta(added={3}, removed={1})])) == [1, 2, 3]
    assert bg.run_graph(emit_tsl, [{0: 1, 1: 10}, {0: 2}, {1: bg.INVALID}, {1: 20}]) == [1, 10, 2, 20]
    assert bg.run_graph(emit_tsd, [{1: 10}, {2: 20}]) == [11, 22]


def test_emit_dictionary_output_uses_the_public_key_value_schema():
    @bg.compute_node
    def read(value: bg.TSB[bg.KeyValue]) -> bg.TS[str]:
        return f"{value.key.value}:{value.value.value}"

    @bg.graph
    def emit_dictionary(value: bg.TS[dict[str, int]]) -> bg.TS[str]:
        return read(bg.operators.emit(value))

    assert bg.run_graph(emit_dictionary, [{"first": 10}, {"second": 20}]) == ["first:10", "second:20"]

    @bg.graph
    def emit_tsd(value: bg.TSD[str, bg.TS[int]]) -> bg.TS[str]:
        return read(bg.operators.emit(value))

    assert bg.run_graph(emit_tsd, [{"first": 10}, {"second": 20}]) == ["first:10", "second:20"]

    @bg.schema
    class UnrelatedKeyValue:
        key: bg.TS[str]
        value: bg.TS[int]

    @bg.compute_node
    def read_unrelated(value: bg.TSB[UnrelatedKeyValue]) -> bg.TS[str]:
        return f"{value.key.value}:{value.value.value}"

    @bg.graph
    def incompatible(value: bg.TS[dict[str, int]]) -> bg.TS[str]:
        return read_unrelated(bg.operators.emit(value))

    with pytest.raises(ValueError, match="does not satisfy its generic type"):
        bg.run_graph(incompatible, [{"first": 10}])
