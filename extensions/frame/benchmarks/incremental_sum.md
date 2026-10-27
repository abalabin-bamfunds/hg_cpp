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

Times cover one `RuntimeGraph::evaluate()` call, including source mutation,
filtering where applicable, sum, downstream publication, and cycle cleanup.
Allocation during evaluation is included. Graph construction, initialization,
and destruction are excluded. Each case runs three graph instances, with ten
warmup cycles and 101 measured cycles per instance. Median and nearest-rank p95
use the combined 303 samples.

## Configuration

- Date: 2026-10-27.
- CPU: AMD EPYC 7R13 virtual machine.
- Compiler: GCC 14.3.0 from `hg-build`.
- BG and frame extension: matching Release builds, `-O3 -DNDEBUG`.
- Frame source: incremental sum changes on base `5169a8d5`.
- Arrow 24 is enabled in the build; the measured graphs use native batches.

## Results

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
```
