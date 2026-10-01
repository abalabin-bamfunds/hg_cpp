"""Compare callback-scoped Dict mappings with eager Python dict conversion.

Run from the repository root with:

    PYTHONPATH=build/python313/bindings/python \
      /home/abalabin/miniforge3/envs/bg-build/bin/python \
      bindings/python/benchmarks/dict_mapping.py

The build should use CMAKE_BUILD_TYPE=Release. Access timings are taken inside
the callback and exclude graph setup and scheduling. Scalar-to-Python dict
conversion is measured separately inside a compute-node callback. The report
includes C++ and Python wrapper sizes, including the cursor's fixed batch
buffer, so its constant iterator memory can be compared with the previous
size-proportional pointer snapshots. Python object sizes are shallow and do
not include the separately allocated native cursor.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from collections.abc import Callable, Mapping

import bg
from bg._bg import _dict_view_layout


def timed(function: Callable[[], object], repeats: int, rounds: int = 7) -> float:
    function()
    measurements = []
    for _ in range(rounds):
        started = time.perf_counter_ns()
        for _ in range(repeats):
            function()
        measurements.append((time.perf_counter_ns() - started) / repeats)
    return statistics.median(measurements)


def access_metrics(mapping: Mapping[int, int], size: int, repeats: int) -> dict[str, float]:
    last_key = size - 1
    missing_key = size + 1
    partial_count = min(8, size)
    traversal_repeats = max(1, min(100, 1_000_000 // max(1, size)))
    key_view = mapping.keys()
    item_view = mapping.items()
    value_view = mapping.values()

    def partial_access() -> int:
        return sum(mapping[index] for index in range(partial_count))

    def getitem_miss() -> None:
        try:
            mapping[missing_key]
        except KeyError:
            pass

    def keys_iteration() -> int:
        return sum(1 for _ in key_view)

    def items_iteration() -> int:
        return sum(key + value for key, value in item_view)

    def values_iteration() -> int:
        return sum(value_view)

    def full_lookup_iteration() -> int:
        return sum(mapping[key] for key in mapping)

    return {
        "mapping_wrapper_bytes": float(sys.getsizeof(mapping)),
        "keys_view_wrapper_bytes": float(sys.getsizeof(key_view)),
        "items_view_wrapper_bytes": float(sys.getsizeof(item_view)),
        "values_view_wrapper_bytes": float(sys.getsizeof(value_view)),
        "mapping_iterator_wrapper_bytes": float(sys.getsizeof(iter(mapping))),
        "keys_view_iterator_wrapper_bytes": float(sys.getsizeof(iter(key_view))),
        "items_view_iterator_wrapper_bytes": float(sys.getsizeof(iter(item_view))),
        "values_view_iterator_wrapper_bytes": float(sys.getsizeof(iter(value_view))),
        "len_ns": timed(lambda: len(mapping), repeats),
        "getitem_hit_ns": timed(lambda: mapping[last_key], repeats),
        "getitem_miss_ns": timed(getitem_miss, repeats),
        "get_hit_ns": timed(lambda: mapping.get(last_key), repeats),
        "get_miss_ns": timed(lambda: mapping.get(missing_key), repeats),
        "contains_hit_ns": timed(lambda: last_key in mapping, repeats),
        "contains_miss_ns": timed(lambda: missing_key in mapping, repeats),
        "partial_8_access_ns": timed(partial_access, repeats),
        "keys_iteration_ns": timed(keys_iteration, traversal_repeats),
        "items_iteration_ns": timed(items_iteration, traversal_repeats),
        "values_iteration_ns": timed(values_iteration, traversal_repeats),
        "full_lookup_iteration_ns": timed(full_lookup_iteration, traversal_repeats),
        "dict_materialization_ns": timed(lambda: dict(mapping), 1),
        "copy_materialization_ns": timed(mapping.copy, 1),
    }


def make_lazy_graph(size: int, repeats: int, measurements: dict[str, float]):
    @bg.node
    def access(values: dict[int, int], trigger: int) -> int:
        if not measurements:
            measurements.update(access_metrics(values, size, repeats))
        return trigger

    @bg.graph
    def graph(data: bg.TS[dict[int, int]], trigger: bg.TS[int]) -> bg.TS[int]:
        return access(data, trigger)

    return graph


def make_eager_graph(size: int, repeats: int, measurements: dict[str, float]):
    @bg.compute_node
    def access(data: bg.TS[dict[int, int]], trigger: bg.TS[int]) -> bg.TS[int]:
        if "scalar_to_python_dict_ns" not in measurements:
            values = data.value
            measurements["scalar_to_python_dict_ns"] = timed(lambda: data.value, 1)
            measurements.update(
                {f"eager_{name}": value for name, value in access_metrics(values, size, repeats).items()}
            )
        return trigger.value

    @bg.graph
    def graph(data: bg.TS[dict[int, int]], trigger: bg.TS[int]) -> bg.TS[int]:
        return access(data, trigger)

    return graph


def benchmark_size(size: int, repeats: int) -> list[tuple[str, float, str]]:
    values = {index: index * 3 for index in range(size)}
    python_metrics = access_metrics(values, size, repeats)

    lazy_metrics: dict[str, float] = {}
    lazy_graph = make_lazy_graph(size, repeats, lazy_metrics)
    bg.test_graph(lazy_graph, [values], [0])

    eager_metrics: dict[str, float] = {}
    eager_graph = make_eager_graph(size, repeats, eager_metrics)
    bg.test_graph(eager_graph, [values], [0])

    result = []
    result.extend((f"python_dict_{name}", value, "ns/op") for name, value in python_metrics.items())
    result.extend((f"lazy_view_{name}", value, "ns/op") for name, value in lazy_metrics.items())
    result.extend((name, value, "ns/op") for name, value in eager_metrics.items())
    _, _, _, _, batch_entry_size, batch_capacity, pointer_size = _dict_view_layout()
    result.extend(
        [
            ("previous_key_value_snapshot_bytes", size * pointer_size, "bytes/iteration"),
            ("previous_item_snapshot_bytes", size * batch_entry_size, "bytes/iteration"),
            ("cursor_batch_bytes", batch_capacity * batch_entry_size, "bytes/iterator"),
        ]
    )
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", type=int, nargs="+", default=[32, 1_000, 100_000])
    parser.add_argument("--repeats", type=int, default=10_000)
    arguments = parser.parse_args()

    view_size, cursor_iterator_size, entry_view_size, scalar_size, batch_entry_size, batch_capacity, _ = (
        _dict_view_layout()
    )
    print(
        f"python={sys.version.split()[0]} DictView_native_bytes={view_size} "
        f"cursor_iterator_native_bytes={cursor_iterator_size} "
        f"entry_view_native_bytes={entry_view_size} Scalar_native_bytes={scalar_size} "
        f"cursor_batch_entries={batch_capacity} cursor_batch_payload_bytes={batch_capacity * batch_entry_size} "
        f"batch_entry_native_bytes={batch_entry_size}"
    )
    print("size,metric,value,unit")
    for size in arguments.sizes:
        for metric, value, unit in benchmark_size(size, arguments.repeats):
            if metric.endswith("_bytes"):
                unit = "bytes/iteration" if "snapshot" in metric else "bytes/iterator"
                if "wrapper" in metric:
                    unit = "bytes/object"
            print(f"{size},{metric},{value:.1f},{unit}")


if __name__ == "__main__":
    main()
