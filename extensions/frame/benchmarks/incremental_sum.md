# Incremental column sum benchmark

## Measurement

[The benchmark](incremental_sum_benchmark.cpp) compares `operators::sum_` with
an ordinary node that sums every current cell through native column batches.
Both graphs use the same `i64` source workload and validate every result outside
the timed interval.

Each measured cycle increments the first row. The direct case uses a native TSC.
The filtered case uses a keyed TSF, selects alternating rows with a fixed
predicate, and sums its value column. The changed row remains selected.
Active rows are the rows presented to the sum.

Complete-cycle times cover one `RuntimeGraph::evaluate()` call, including source mutation,
filtering where applicable, sum, downstream publication, and cycle cleanup.
Allocation during evaluation is included. Graph construction, initialization,
and destruction are excluded. Each case runs three graph instances, with ten
warmup cycles and 101 measured cycles per instance. Median and nearest-rank p95
use the combined 303 samples.

## Configuration

- Complete-cycle measurements: 2026-10-27.
- Sum-node measurements: 2026-10-28.
- CPU: AMD EPYC 7R13 virtual machine.
- Compiler: GCC 14.3.0 from `hg-build`.
- BG and frame extension: matching Release builds, `-O3 -DNDEBUG`.
- Frame source: incremental sum changes on base `5169a8d5`; the sum-node
  profiling harness starts from `3f23e24c`.
- Arrow 24 is enabled in the build; the measured graphs use native batches.

## Complete-cycle results

Times are milliseconds per complete evaluation cycle. Ratios compare medians.
[Raw CSV](results/2026-10-27/incremental-sum.csv).

| Input | Source rows | Active rows | Incremental median | Incremental p95 | Full scan median | Full scan p95 | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Column | 1,000 | 1,000 | 0.000770 | 0.000820 | 0.002370 | 0.002380 | 3.08× |
| Column | 10,000 | 10,000 | 0.000990 | 0.001030 | 0.019970 | 0.033031 | 20.17× |
| Column | 100,000 | 100,000 | 0.003020 | 0.003460 | 0.198174 | 0.206323 | 65.62× |
| Filtered | 1,000 | 500 | 0.002120 | 0.002151 | 0.008740 | 0.008760 | 4.12× |
| Filtered | 10,000 | 5,000 | 0.002660 | 0.002700 | 0.072002 | 0.072771 | 27.07× |
| Filtered | 100,000 | 50,000 | 0.007890 | 0.007970 | 0.588571 | 0.603751 | 74.60× |

## Sum-node results

The `--profile-nodes` mode records BG's `EvaluationProfile` and reads
`EvaluationProfiler::node_totals()` after each cycle. The selected sum node's
wiring identity is mapped to its compiled node index. Each sample requires
exactly one evaluation of that node. Reader processing, result validation, and
profiler queries run between evaluations. Warmup and sample counts match the
complete-cycle measurement.

These intervals exclude source, filter, and sink evaluation and cycle cleanup.
BG checkpoints occur before output publication, so each interval includes
intervening publication and scheduling and ends before the sum's publication.
Profiling overhead is included. See BG's
[profiling contract](../../../docs/evaluation.md#evaluation-profiling).

On this x86 machine, checkpoints use the timestamp counter. Each repeat
converts ticks to time using the steady-clock and checkpoint readings from the
first measured cycle's start to the last measured cycle's end. The profiled
and complete-cycle tables come from separate runs.

Times below are **microseconds per sum-node evaluation**. Ratios compare
medians. The [CSV](results/2026-10-28/incremental-sum-nodes.csv) retains native
tick summaries, converted milliseconds, and calibration summaries from the
three repeats. Calibration was approximately 2,649,953 ticks per millisecond.

| Input | Source rows | Active rows | Incremental median | Incremental p95 | Full scan median | Full scan p95 | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Column | 1,000 | 1,000 | 0.440 | 0.480 | 1.810 | 1.820 | 4.11× |
| Column | 10,000 | 10,000 | 0.660 | 0.690 | 16.910 | 17.680 | 25.62× |
| Column | 100,000 | 100,000 | 2.600 | 2.660 | 169.423 | 180.853 | 65.16× |
| Filtered | 1,000 | 500 | 0.570 | 0.600 | 7.310 | 7.730 | 12.82× |
| Filtered | 10,000 | 5,000 | 0.780 | 0.800 | 71.161 | 72.211 | 91.23× |
| Filtered | 100,000 | 50,000 | 2.740 | 4.730 | 587.201 | 604.551 | 214.30× |

## Interpretation

These measurements cover one changed row per cycle. Bulk changes, frequent
rebinding, floating-point overflow recovery, and initialization have different
costs. The incremental node retains previous non-null contributions by row ID.
Bitmap traversal still depends on the allocated row-ID range.
The filtered full scan traverses the alternating-row selection each cycle;
incremental updates avoid that complete traversal.

## Run

Configure a Release frame build with `BG_FRAME_BUILD_BENCHMARKS=ON`, then run:

```sh
cmake --build <release-build> --target bg_frame_incremental_sum_benchmark -j12
<release-build>/bg_frame_incremental_sum_benchmark
<release-build>/bg_frame_incremental_sum_benchmark --profile-nodes
```
