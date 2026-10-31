import argparse
import ctypes
import gc
import os
import platform
import statistics
import sys
import time
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc


SEED = 20_260_928
PHYSICAL_ROWS = 100_000
DEAD_RATIOS = (0.0, 0.0001, 0.001, 0.01, 0.20, 0.80)
SEED_OFFSETS = {0.0: 0, 0.0001: 404, 0.001: 505, 0.01: 101, 0.20: 202, 0.80: 303}
VALUE_TYPE = pa.int64()


class ChunkDescriptor(ctypes.Structure):
    _fields_ = [("values", ctypes.c_size_t), ("length", ctypes.c_size_t)]


def cpu_model():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def live_positions(row_count, dead_ratio, pattern, seed):
    dead_count = round(row_count * dead_ratio)
    if dead_count == 0:
        return np.arange(row_count, dtype=np.uint32)

    if pattern == "clustered":
        dead_start = (row_count - dead_count) // 2
        dead = np.arange(dead_start, dead_start + dead_count, dtype=np.uint32)
    else:
        generator = np.random.default_rng(seed)
        dead = np.sort(generator.choice(row_count, size=dead_count, replace=False).astype(np.uint32))

    live = np.ones(row_count, dtype=np.bool_)
    live[dead] = False
    return np.flatnonzero(live).astype(np.uint32)


def count_runs(positions):
    if len(positions) == 0:
        return 0
    return 1 + int(np.count_nonzero(positions[1:] != positions[:-1] + 1))


def measure(function, warmups, repetitions):
    for _ in range(warmups):
        result = function()
        del result

    samples_ms = []
    for _ in range(repetitions):
        start = time.perf_counter_ns()
        result = function()
        samples_ms.append((time.perf_counter_ns() - start) / 1_000_000)
        del result

    return statistics.median(samples_ms), min(samples_ms), max(samples_ms)


def timing_text(samples):
    median, minimum, maximum = samples
    return f"{median:.3f} [{minimum:.3f}, {maximum:.3f}]"


def hardware_report():
    return [
        f"- Platform: `{platform.platform()}`",
        f"- CPU: `{cpu_model()}` ({os.cpu_count()} logical CPUs)",
        f"- Python: `{platform.python_version()}`",
        f"- PyArrow: `{pa.__version__}`",
        f"- NumPy: `{np.__version__}`",
        f"- Fixed seed: `{SEED}`",
        f"- Physical rows per case: `{PHYSICAL_ROWS:,}`",
        "- Warmups: 1 for each sparse export, 5 for BG copy and dense exports, 2 for compute; repetitions: "
        "3 when chunk count is at least 10,000, otherwise 5 for each sparse export; 31 for BG copy and "
        "each dense export; "
        "7 for compute",
        "- Exact command: `conda run -n bg-build bash "
        "extensions/frame/archive/experiments/frame_arrow_probe/benchmark.sh`",
    ]


def run_case(library, positions, ratio, pattern):
    live_count = len(positions)
    position_pointer = positions.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32))
    library.benchmark_create(PHYSICAL_ROWS, position_pointer, live_count)
    try:
        assert library.benchmark_live_count() == live_count

        descriptors = (ChunkDescriptor * live_count)()
        dense_values = np.empty(live_count, dtype=np.int64)
        dense_output = dense_values.ctypes.data_as(ctypes.POINTER(ctypes.c_int64))

        def sparse_export(output=descriptors):
            chunk_count = library.benchmark_export_chunks(output, live_count)
            arrays = []
            for index in range(chunk_count):
                descriptor = output[index]
                buffer = pa.foreign_buffer(descriptor.values, descriptor.length * 8, base=library)
                arrays.append(
                    pa.Array.from_buffers(VALUE_TYPE, descriptor.length, [None, buffer], null_count=0)
                )
            return pa.chunked_array(arrays, type=VALUE_TYPE)

        def sparse_export_with_allocation():
            return sparse_export((ChunkDescriptor * live_count)())

        def dense_export():
            copied = library.benchmark_copy_live_values(dense_output)
            assert copied == live_count
            buffer = pa.foreign_buffer(dense_values.ctypes.data, dense_values.nbytes, base=dense_values)
            return pa.Array.from_buffers(VALUE_TYPE, live_count, [None, buffer], null_count=0)

        def dense_export_with_allocation():
            output = np.empty(live_count, dtype=np.int64)
            copied = library.benchmark_copy_live_values(
                output.ctypes.data_as(ctypes.POINTER(ctypes.c_int64))
            )
            assert copied == live_count
            buffer = pa.foreign_buffer(output.ctypes.data, output.nbytes, base=output)
            return pa.Array.from_buffers(VALUE_TYPE, live_count, [None, buffer], null_count=0)

        sparse_values = sparse_export()
        sparse_allocated_values = sparse_export_with_allocation()
        dense_values_arrow = dense_export()
        allocated_values_arrow = dense_export_with_allocation()
        sparse_chunks = sparse_values.num_chunks
        assert sparse_chunks == library.benchmark_export_chunks(descriptors, live_count)
        assert len(sparse_values) == len(sparse_allocated_values) == len(dense_values_arrow) == live_count
        assert len(allocated_values_arrow) == live_count

        expected_values = ((positions.astype(np.uint64) * 17 + 13) % 1009).astype(np.int64)
        expected_sum = int(expected_values.sum(dtype=np.int64))
        assert sparse_values.to_pylist() == expected_values.tolist()
        assert sparse_allocated_values.to_pylist() == expected_values.tolist()
        assert dense_values_arrow.to_pylist() == expected_values.tolist()
        assert allocated_values_arrow.to_pylist() == expected_values.tolist()

        keep = np.arange(live_count) % 4 == 0
        filter_mask = pa.array(keep)
        expected_filtered = expected_values[keep].tolist()
        assert pc.sum(sparse_values).as_py() == pc.sum(dense_values_arrow).as_py() == expected_sum
        assert pc.multiply(sparse_values, 3).to_pylist() == pc.multiply(dense_values_arrow, 3).to_pylist()
        assert pc.filter(sparse_values, filter_mask).to_pylist() == expected_filtered
        assert pc.filter(dense_values_arrow, filter_mask).to_pylist() == expected_filtered

        export_repetitions = 3 if sparse_chunks >= 10_000 else 5
        sparse_export_time = measure(sparse_export, 1, export_repetitions)
        sparse_allocated_time = measure(sparse_export_with_allocation, 1, export_repetitions)
        copy_time = measure(lambda: library.benchmark_copy_live_values(dense_output), 5, 31)
        dense_export_time = measure(dense_export, 5, 31)
        dense_allocated_time = measure(dense_export_with_allocation, 5, 31)
        sparse_sum_time = measure(lambda: pc.sum(sparse_values), 2, 7)
        dense_sum_time = measure(lambda: pc.sum(dense_values_arrow), 2, 7)
        sparse_filter_time = measure(lambda: pc.filter(sparse_values, filter_mask), 2, 7)
        dense_filter_time = measure(lambda: pc.filter(dense_values_arrow, filter_mask), 2, 7)

        return {
            "ratio": ratio,
            "pattern": pattern,
            "live": live_count,
            "dead": PHYSICAL_ROWS - live_count,
            "runs": count_runs(positions),
            "chunks": sparse_chunks,
            "export_repetitions": export_repetitions,
            "sparse_export": timing_text(sparse_export_time),
            "sparse_allocated": timing_text(sparse_allocated_time),
            "copy": timing_text(copy_time),
            "dense_export": timing_text(dense_export_time),
            "dense_allocated": timing_text(dense_allocated_time),
            "sparse_sum": timing_text(sparse_sum_time),
            "dense_sum": timing_text(dense_sum_time),
            "sparse_filter": timing_text(sparse_filter_time),
            "dense_filter": timing_text(dense_filter_time),
            "sparse_export_median": sparse_export_time[0],
            "sparse_allocated_median": sparse_allocated_time[0],
            "dense_export_median": dense_export_time[0],
            "dense_allocated_median": dense_allocated_time[0],
            "sparse_sum_median": sparse_sum_time[0],
            "dense_sum_median": dense_sum_time[0],
            "sparse_filter_median": sparse_filter_time[0],
            "dense_filter_median": dense_filter_time[0],
        }
    finally:
        library.benchmark_destroy()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True)
    parser.add_argument("--output")
    parser.add_argument("--reverse-patterns", action="store_true")
    arguments = parser.parse_args()

    library = ctypes.CDLL(arguments.library)
    library.benchmark_create.argtypes = [
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_size_t,
    ]
    library.benchmark_create.restype = None
    library.benchmark_destroy.restype = None
    library.benchmark_live_count.restype = ctypes.c_size_t
    library.benchmark_export_chunks.argtypes = [
        ctypes.POINTER(ChunkDescriptor),
        ctypes.c_size_t,
    ]
    library.benchmark_export_chunks.restype = ctypes.c_size_t
    library.benchmark_copy_live_values.argtypes = [ctypes.POINTER(ctypes.c_int64)]
    library.benchmark_copy_live_values.restype = ctypes.c_size_t

    gc.disable()
    report = [
        "# Sparse chunk export benchmark",
        "",
        *hardware_report(),
        "",
        "Timings are milliseconds as `median [minimum, maximum]` over repeated runs.",
        "Sparse export scans occupied positions, discovers BG spans, and creates PyArrow chunks; its timing includes "
        "Python per-chunk wrapper construction. The allocation variant also allocates a zero-filled descriptor "
        "array with capacity for every live row.",
        "BG copy visits live positions and copies values into a preallocated NumPy buffer. "
        "Dense export adds PyArrow wrapping to that copy. "
        "Dense export with allocation creates a fresh NumPy buffer inside each timed call.",
        "Compute inputs are built before timing. The single-chunk baseline is the same live values. A native adapter "
        "could reduce sparse export wrapper cost; Arrow compute still sees the resulting chunk count.",
        "",
        "| Dead ratio | Pattern | Live | Dead | Live runs | Arrow chunks | Sparse reps | Sparse preallocated | "
        "Sparse with allocation | "
        "BG copy | Dense preallocated | Dense with allocation | Sum sparse | Sum single | Filter sparse | Filter single |",
        "|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]

    cases = []
    for ratio in DEAD_RATIOS:
        patterns = ("clustered",) if ratio == 0 else ("clustered", "scattered")
        if arguments.reverse_patterns:
            patterns = tuple(reversed(patterns))
        for pattern in patterns:
            positions = live_positions(
                PHYSICAL_ROWS,
                ratio,
                pattern,
                SEED + SEED_OFFSETS[ratio] + (pattern == "scattered"),
            )
            result = run_case(library, positions, ratio, pattern)
            cases.append(result)
            report.append(
                (
                    "| {ratio:.4%} | {pattern} | {live:,} | {dead:,} | {runs:,} | {chunks:,} | "
                    "{export_repetitions} | {sparse_export} | {sparse_allocated} | {copy} | "
                    "{dense_export} | {dense_allocated} | "
                    "{sparse_sum} | {dense_sum} | "
                    "{sparse_filter} | {dense_filter} |"
                ).format(**result)
            )

    report.extend(
        [
            "",
            "All cases assert identical values, sums, multiplied values, and filtered outputs before timing.",
            "Chunk count includes splits at deleted-row gaps and BG value-block boundaries.",
            "",
            "## Derived one-export plus one-operator comparison",
            "",
            "Each total adds the separately measured export and operator medians; the combined operation was not timed directly. "
            "The sparse path allocates a descriptor array and the dense path allocates a value array.",
            "| Dead ratio | Pattern | Chunks | Export + sum sparse/dense (ms) | Relative | "
            "Export + filter sparse/dense (ms) | Relative |",
            "|---:|:---|---:|---:|---:|---:|---:|",
        ]
    )
    for result in cases:
        sparse_sum_total = result["sparse_allocated_median"] + result["sparse_sum_median"]
        dense_sum_total = result["dense_allocated_median"] + result["dense_sum_median"]
        sparse_filter_total = result["sparse_allocated_median"] + result["sparse_filter_median"]
        dense_filter_total = result["dense_allocated_median"] + result["dense_filter_median"]
        report.append(
            f"| {result['ratio']:.4%} | {result['pattern']} | {result['chunks']:,} | "
            f"{sparse_sum_total:.3f} / {dense_sum_total:.3f} | {sparse_sum_total / dense_sum_total:.2f}x | "
            f"{sparse_filter_total:.3f} / {dense_filter_total:.3f} | "
            f"{sparse_filter_total / dense_filter_total:.2f}x |"
        )
    output = "\n".join(report) + "\n"
    print(output, end="")
    if arguments.output:
        Path(arguments.output).write_text(output)


if __name__ == "__main__":
    main()
