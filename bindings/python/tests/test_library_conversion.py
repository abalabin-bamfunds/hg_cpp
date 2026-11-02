import hg
import pytest


@hg.schema
class ScalarRecord:
    count: int
    ratio: float


@hg.schema
class SeriesRecord:
    ratio: hg.TS[float]
    count: hg.TS[int]


def test_convert_copies_fixed_tsl_snapshots_to_and_from_scalar_lists():
    @hg.compute_node
    def sum_list(value: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return sum(value.value)

    @hg.graph
    def list_to_tsl(value: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        snapshot = hg.operators.convert[hg.TSL[hg.TS[int], 2]](value)
        return sum_list(hg.operators.convert[hg.TS[tuple[int, ...]]](snapshot))

    @hg.graph
    def tsl_to_list(value: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return sum_list(hg.operators.convert[hg.TS[tuple[int, ...]]](value))

    assert hg.run_graph(list_to_tsl, [[1, 2], [3, 4]]) == [3, 7]
    assert hg.run_graph(tsl_to_list, [{0: 1, 1: 2}, {0: 3}]) == [3, 5]


def test_convert_copies_scalar_list_and_dictionary_collection_snapshots():
    @hg.compute_node
    def sum_tss(value: hg.TSS[int]) -> hg.TS[int]:
        return sum(value.value)

    @hg.graph
    def list_to_tss(value: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return sum_tss(hg.operators.convert[hg.TSS[int]](value))

    @hg.compute_node
    def sum_dict(value: hg.TS[dict[int, int]]) -> hg.TS[int]:
        return sum(value.value.values())

    @hg.compute_node
    def sum_tsd(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return sum(value.value.values())

    @hg.graph
    def tsd_to_dict(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        return sum_dict(hg.operators.convert[hg.TS[dict[int, int]]](value))

    @hg.graph
    def dict_to_tsd(value: hg.TS[dict[int, int]]) -> hg.TS[int]:
        return sum_tsd(hg.operators.convert[hg.TSD[int, hg.TS[int]]](value))

    assert hg.run_graph(list_to_tss, [[1, 2], [2, 3]]) == [3, 5]
    assert hg.run_graph(tsd_to_dict, [{1: 10, 2: 20}, {2: 7}, {2: hg.REMOVE}]) == [30, 17, 10]
    assert hg.run_graph(dict_to_tsd, [{1: 10, 2: 20}, {2: 7}, {}]) == [30, 7, 0]


def test_convert_maps_runtime_bundle_and_tsb_schemas_by_field_name():
    @hg.compute_node
    def summarize(value: hg.TSB[SeriesRecord]) -> hg.TS[float]:
        return value.count.value + value.ratio.value

    @hg.graph
    def bundle_to_tsb(value: hg.TS[hg.Bundle[ScalarRecord]]) -> hg.TS[float]:
        converted = hg.operators.convert[hg.TSB[SeriesRecord]](value)
        return summarize(converted)

    @hg.compute_node
    def read_bundle(value: hg.TS[hg.Bundle[ScalarRecord]]) -> hg.TS[float]:
        return value.value.count + value.value.ratio

    @hg.graph
    def tsb_to_bundle(value: hg.TSB[SeriesRecord]) -> hg.TS[float]:
        converted = hg.operators.convert[hg.TS[hg.Bundle[ScalarRecord]]](value)
        return read_bundle(converted)

    assert hg.run_graph(bundle_to_tsb, [ScalarRecord(count=2, ratio=0.5)]) == [2.5]
    assert hg.run_graph(tsb_to_bundle, [{"ratio": 0.5, "count": 2}]) == [2.5]


def test_combine_publishes_required_and_initially_partial_unmarked_values():
    @hg.graph
    def combine_tsl(first: hg.TS[int], second: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        return hg.operators.combine[hg.TSL[hg.TS[int], 2]](first, second)

    @hg.graph
    def combine_named(count: hg.TS[int], ratio: hg.TS[float]) -> hg.TSB[SeriesRecord]:
        return hg.operators.combine[hg.TSB[SeriesRecord]](ratio=ratio, count=count)

    @hg.graph
    def combine_partial_tsl(first: hg.TS[int], second: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        return hg.operators.combine[hg.TSL[hg.TS[int], 2]](first, second)

    @hg.graph
    def combine_partial_named(count: hg.TS[int], ratio: hg.TS[float]) -> hg.TSB[SeriesRecord]:
        return hg.operators.combine[hg.TSB[SeriesRecord]](ratio=ratio, count=count)

    @hg.graph
    def combine_optional(count: hg.TS[int], ratio: hg.TS[float]) -> hg.TSB[SeriesRecord]:
        return hg.operators.combine[hg.TSB[SeriesRecord]](count=hg.opt(count), ratio=hg.opt(ratio))

    assert hg.run_graph(combine_tsl, [1, 2], [10, None]) == [{0: 1, 1: 10}, {0: 2}]
    assert hg.run_graph(combine_named, [7, 8], [2.5, None]) == [
        {"count": 7, "ratio": 2.5},
        {"count": 8},
    ]
    assert hg.run_graph(combine_partial_tsl, [None, 2], [10, None]) == [{1: 10}, {0: 2}]
    assert hg.run_graph(combine_partial_named, [None, 7], [2.5, None]) == [
        {"ratio": 2.5},
        {"count": 7},
    ]
    assert hg.run_graph(combine_optional, [hg.INVALID, 7, hg.INVALID, 9], [2.5, None, hg.INVALID, 3.5]) == [
        {"ratio": 2.5},
        {"count": 7},
        None,
        {"count": 9, "ratio": 3.5},
    ]


def test_collect_accumulates_collection_snapshots_and_preserves_history_on_invalidation():
    @hg.graph
    def collect_list(value: hg.TS[int]) -> hg.TS[tuple[int, ...]]:
        return hg.operators.collect[hg.TS[tuple[int, ...]]](value)

    @hg.graph
    def collect_dict(key: hg.TS[int], value: hg.TS[int]) -> hg.TS[dict[int, int]]:
        return hg.operators.collect[hg.TS[dict[int, int]]](key=key, value=value)

    assert hg.run_graph(collect_list, [1, 1, hg.INVALID, 2]) == [(1,), (1, 1), None, (1, 1, 2)]
    assert hg.run_graph(collect_dict, [1, 1, 2], [10, 11, hg.INVALID]) == [
        {1: 10},
        {1: 11},
        None,
    ]

    @hg.graph
    def collect_list_with_reset(value: hg.TS[int], reset: hg.TS[bool]) -> hg.TS[tuple[int, ...]]:
        return hg.operators.collect[hg.TS[tuple[int, ...]]](value, reset=reset)

    assert hg.run_graph(collect_list_with_reset, [1, 2, 3, 4], [None, None, False, None]) == [
        (1,),
        (1, 2),
        (3,),
        (3, 4),
    ]

    @hg.graph
    def collect_set_with_reset(value: hg.TS[int], reset: hg.TS[bool]) -> hg.TSS[int]:
        return hg.operators.collect[hg.TSS[int]](value, reset=reset)

    assert hg.run_graph(collect_set_with_reset, [1, 2, 3, 4], [None, None, False, None]) == [
        hg.SetDelta(added={1}),
        hg.SetDelta(added={2}),
        hg.SetDelta(added={3}, removed={1, 2}),
        hg.SetDelta(added={4}),
    ]

    @hg.graph
    def collect_tsd_with_reset(key: hg.TS[int], value: hg.TS[int], reset: hg.TS[bool]) -> hg.TSD[int, hg.TS[int]]:
        return hg.operators.collect[hg.TSD[int, hg.TS[int]]](key=key, value=value, reset=reset)

    assert hg.run_graph(collect_tsd_with_reset, [1, 2], [10, 20], [None, False]) == [
        {1: 10},
        {1: hg.REMOVE, 2: 20},
    ]


def test_emit_serializes_scalar_collections_and_dictionary_entries():
    @hg.graph
    def emit_list(value: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return hg.operators.emit(value)

    @hg.graph
    def emit_dictionary(value: hg.TS[dict[int, int]]) -> hg.TS[int]:
        entry = hg.operators.emit(value)
        key = hg.operators.getattr(entry, hg.symbol("key"))
        item = hg.operators.getattr(entry, hg.symbol("value"))
        return key + item

    @hg.graph
    def emit_tsd(value: hg.TSD[int, hg.TS[int]]) -> hg.TS[int]:
        entry = hg.operators.emit(value)
        key = hg.operators.getattr(entry, hg.symbol("key"))
        item = hg.operators.getattr(entry, hg.symbol("value"))
        return key + item

    @hg.graph
    def emit_tss(value: hg.TSS[int]) -> hg.TS[int]:
        return hg.operators.emit(value)

    @hg.graph
    def emit_tsl(value: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return hg.operators.emit(value)

    assert hg.run_graph(emit_list, [[1, 2], [], [3], hg.INVALID]) == [1, 2, 3, None]
    assert hg.run_graph(emit_dictionary, [{1: 10}, {2: 20}]) == [11, 22]
    assert sorted(hg.run_graph(emit_tss, [{1, 2}, hg.SetDelta(added={3}, removed={1})])) == [1, 2, 3]
    assert hg.run_graph(emit_tsl, [{0: 1, 1: 10}, {0: 2}, {1: hg.INVALID}, {1: 20}]) == [1, 10, 2, 20]
    assert hg.run_graph(emit_tsd, [{1: 10}, {2: 20}]) == [11, 22]


def test_emit_dictionary_output_uses_the_public_key_value_schema():
    @hg.compute_node
    def read(value: hg.TSB[hg.KeyValue]) -> hg.TS[str]:
        return f"{value.key.value}:{value.value.value}"

    @hg.graph
    def emit_dictionary(value: hg.TS[dict[str, int]]) -> hg.TS[str]:
        return read(hg.operators.emit(value))

    assert hg.run_graph(emit_dictionary, [{"first": 10}, {"second": 20}]) == ["first:10", "second:20"]

    @hg.graph
    def emit_tsd(value: hg.TSD[str, hg.TS[int]]) -> hg.TS[str]:
        return read(hg.operators.emit(value))

    assert hg.run_graph(emit_tsd, [{"first": 10}, {"second": 20}]) == ["first:10", "second:20"]

    @hg.schema
    class UnrelatedKeyValue:
        key: hg.TS[str]
        value: hg.TS[int]

    @hg.compute_node
    def read_unrelated(value: hg.TSB[UnrelatedKeyValue]) -> hg.TS[str]:
        return f"{value.key.value}:{value.value.value}"

    @hg.graph
    def incompatible(value: hg.TS[dict[str, int]]) -> hg.TS[str]:
        return read_unrelated(hg.operators.emit(value))

    with pytest.raises(ValueError, match="does not satisfy its generic type"):
        hg.run_graph(incompatible, [{"first": 10}])
