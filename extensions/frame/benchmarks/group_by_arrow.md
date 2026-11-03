# Grouped aggregation: BG and Arrow Compute

## Comparison

The benchmark applies the same grouping and sums to the same sequence of
100,000-row frame values. BG retains row membership and contributions between
evaluations. Arrow rebuilds its groups from the current frame value on each
evaluation. Small updates therefore compare different amounts of work;
scalar-parameter updates make BG recalculate every row's contribution.

BG publishes an incremental native frame and retains aggregation state for the
next evaluation. Arrow returns a new full result table; converting that result
to a native frame is outside this comparison.

Arrow uses the public Acero aggregate node with Compute's `hash_sum` kernel.
Execution uses `use_threads=false`. Conversion to double and multiplication
for captured scalar parameters execute in an Arrow projection within the
measured query. The two-sum case calculates an integer sum and a double sum.

The input contains one `i64` grouping column and one `i64` value column. Values
and expected sums are exactly representable in both integer and double form.
The benchmark uses the same update schedule and independent final-state
recomputation as the [native group-by benchmark](group_by.md).

Two input layouts distinguish Arrow execution from frame export:

- Borrowed columns retain the frame's chunks without copying cell buffers.
  Each view is created and consumed within its node invocation.
- Dense copied columns use one owned chunk per column. Preparation includes
  allocation and copying from the current frame value.

Only the grouping and value columns are exported. The user-key column used
by the mutation source is not needed by either aggregation.

## Timing

Preparation measures construction of the Arrow input columns and table.
Query time includes Acero plan construction, projection where required,
group-state construction, aggregation, and result allocation. The result is
available when that interval ends. Total time includes preparation, query,
and destruction of input and result tables. BG source mutation and scheduling
are outside these Arrow intervals.

BG node intervals come from the evaluation profiler and include the invocation
and invocation-end callbacks. Checkpoints include preceding publication and
scheduling and end before the node's output publication. Source and sink
evaluation are excluded. The profiling run also records complete-cycle
durations to check node intervals against their corresponding cycles.

Each workload first runs an untimed replay and checks its final group totals
against an independent full recomputation. Timed evaluations check the group
count. Ten warmup updates precede 101 measured updates on each of three graph
instances, giving 303 update samples per case. The initial evaluation is
recorded separately.

## Configuration

- Date: 2026-11-04.
- CPU: AMD EPYC 7R13 virtual machine; no CPU affinity.
- Engine and extension: GCC 14.3, C++23, Release (`-O3 -DNDEBUG`).
- Arrow: 24.0.0, Release, built with GNU 14.3.0; enabled and detected SIMD AVX2.
- Acero: `use_threads=false`.
- Engine dependency and the approved bitmap header overlay match the
  [native benchmark configuration](group_by.md#configuration).
- Comparator: `77b109bcf`, applied to the canonical frame lane. Both benchmark
  executables were built in `frame-groupby-review/build/group-by-arrow` and
  executed sequentially.

## Update results

Times are **milliseconds per evaluation**, over 100,000 source rows. Each
median uses 303 update samples. BG reports the aggregation node interval;
Arrow reports preparation, query, and destruction together. Both exclude
source mutation and sink evaluation. All result-oracle checks passed.

| Workload | Changed rows | Groups | Sums | BG node | Arrow borrowed | Arrow dense copy |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Value update | 1 | 100 | 1 | 0.024 | 1.456 | 2.400 |
| Value update | 100 | 100 | 1 | 0.058 | 1.461 | 2.409 |
| Value update | 10,000 | 100 | 1 | 0.915 | 1.543 | 2.427 |
| Value update | 1 | 10,000 | 1 | 0.020 | 2.223 | 3.105 |
| Value update | 100 | 10,000 | 1 | 0.051 | 2.112 | 3.381 |
| Value update | 10,000 | 10,000 | 1 | 3.771 | 2.381 | 3.174 |
| Key move | 100 | 100 | 1 | 0.068 | 1.463 | 2.493 |
| Remove 100 / add 100 | 100 + 100 | 100 | 1 | 0.105 | 1.495 | 2.432 |
| Value update | 1 | 100 | 2 | 0.037 | 2.006 | 2.882 |
| Scalar parameter | 0 | 100 | 1 | 5.472 | 1.804 | 2.760 |

A scalar-parameter update recalculates all 100,000 contributions. The two-sum
case updates both aggregations. Changing 10,000 rows affects 100 output groups
in one workload and 10,000 in the other.

The full raw results retain medians and p95 values:
[Arrow phases](results/2026-11-04/group-by-arrow.csv),
[BG node intervals and paired cycles](results/2026-11-04/group-by-comparison-bg-nodes.csv),
and [unprofiled BG cycles](results/2026-11-04/group-by-comparison-bg.csv).

These are separate runs on a virtual machine without CPU affinity. Variation
is substantial: the scalar-update p95 is 16.632 ms for the BG node and 5.187 ms
for Arrow borrowed input. In the one-row, 100-group case, the unprofiled BG
cycle median is 0.012 ms and the profiled cycle median is 0.029 ms. The
measurements do not separate profiling overhead from variation between runs.
Every profiled node interval passed its corresponding cycle-containment check.
The table uses the fresh comparison runs; the earlier native results remain
in their [separate report](group_by.md).

## Export costs

Both input columns have 15 chunks in the borrowed layout and one chunk in the
dense layout throughout the measured updates. For the one-row, 100-group case:

| Input layout | Preparation median | Query median | Total median |
| --- | ---: | ---: | ---: |
| Borrowed | 0.020 ms | 1.430 ms | 1.456 ms |
| Dense copy | 1.121 ms | 1.269 ms | 2.400 ms |

The single-chunk query saves about 0.16 ms, while preparing the dense copy
costs about 1.10 ms more. Gathering does not pay for itself at this chunk count.
Across the workloads, borrowed preparation medians are 0.019–0.028 ms and dense
preparation medians are 1.106–1.191 ms.

## Initial evaluation

Times are milliseconds. Each median has only three samples. BG reports the
complete initial cycle, including construction of retained membership and
contribution state. Arrow reports preparation, query, and destruction.
Source population in `start()` is excluded from both.

| Aggregation | Groups | BG cycle | Arrow borrowed | Arrow dense copy |
| --- | ---: | ---: | ---: | ---: |
| Integer sum | 100 | 8.936 | 1.702 | 3.748 |
| Integer sum | 10,000 | 19.180 | 2.482 | 3.791 |
| Integer and double sums | 100 | 12.227 | 2.200 | 4.045 |
| Scaled double sum | 100 | 15.782 | 2.076 | 4.193 |

## Findings

Small updates benefit substantially from retained membership and contributions.
BG also remains faster when 10,000 changed rows affect only 100 groups. Arrow
is faster when those changes affect 10,000 groups, when a scalar change forces
every contribution to be recalculated, and during initial aggregation.

The incremental path serves small updates well. Processing large updates and
constructing retained state remain performance work. These measurements cover
integer grouping keys and non-null numeric values; they do not establish the
cost of string keys or nullable data.

## Run

Configure a Release frame build with `HG_FRAME_BUILD_ARROW=ON` and
`HG_FRAME_BUILD_BENCHMARKS=ON`. Arrow Compute and Acero CMake packages must be
available, then run:

```sh
cmake --build <release-build> --target hg_frame_group_by_arrow_benchmark hg_frame_group_by_benchmark -j12
<release-build>/hg_frame_group_by_arrow_benchmark --smoke
<release-build>/hg_frame_group_by_arrow_benchmark
<release-build>/hg_frame_group_by_benchmark
<release-build>/hg_frame_group_by_benchmark --profile-nodes
```

Run the timing commands sequentially.
