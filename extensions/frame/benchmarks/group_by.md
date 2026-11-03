# Incremental grouped aggregation benchmark

The [Arrow Compute comparison](group_by_arrow.md) applies the same workloads
through Acero and includes fresh native measurements from the same build.

## Measurement

[The benchmark](group_by_benchmark.cpp) measures `group_by(...).agg(...)` over
100,000 source rows. Grouping uses one non-null `i64` column. Workloads change
numeric values, move rows between groups, remove and add rows, or update a
captured scalar time series. The two-aggregation case sums the same source
column as `i64` and explicitly converted `double`. All measured graphs use
native frame batches; they perform no Arrow export.
Successive update batches start 7,919 row positions apart modulo 100,000;
rows within each batch are consecutive. This exercises the full row-ID range,
including for one-row updates.

Each workload first runs an untimed replay of the complete update sequence. Its
final group totals are checked against an independent full recomputation. Timed
cycles check the group count and use a sink that reads only the result size.

Complete-cycle samples cover one `RuntimeGraph::evaluate()` call, including
source mutation, allocation during evaluation, grouped aggregation, publication,
the sink, and cycle cleanup. Graph construction, source population in `start()`,
and destruction are excluded. Initial samples measure the first evaluation
after source population. Update samples exclude ten warmup cycles per graph.
Three graph instances supply 303 update samples and three initial samples.
Tables use the median and nearest-rank p95.

The `--profile-nodes` run uses HG's evaluation profiler to isolate the grouped
aggregation node. Each sample requires exactly one evaluation of that node.
Checkpoint intervals exclude source and sink evaluation. They include
intervening publication and scheduling before the group node, its invocation
and invocation-end callbacks, and end before its output publication. They
include profiling overhead. Timestamp-counter intervals are converted using
steady-clock cycle anchors for each repeat. These are separate runs from the
complete-cycle measurements. See HG's
[profiling contract](../../../docs/development/design/evaluation.md#evaluation-profiling).

The `--overflow-recovery` run measures only the aggregation adapter's result
sweep. Each group initially receives two `DBL_MAX` contributions. Changing one
to `-DBL_MAX` requires recovery to a sum of zero. Updates and setup run outside
the timer; it covers one `result()` call and result copy per group. Two warmup
sweeps precede nine measured sweeps. This is not a complete graph evaluation.

## Configuration

- Date: 2026-11-04.
- CPU: AMD EPYC 7R13 virtual machine; no CPU affinity.
- Compiler: GCC 14.3, C++23.
- Engine and extension: matching Release builds, `-O3 -DNDEBUG`.
- Engine dependency: `.lanes/frame-free-bundle-field/build/install`, including
  the separately tested free-TSB field-selection fix.
- Frame source: `agent/frame-build` at `29e6b8f43` with the current group-by
  work, review coverage from `69adc01ff`, and numerical fixes from `384de57e7`.
- The approved bitmap union header is supplied from the canonical frame lane's
  dependency overlay. The benchmark lane imports that frame source onto
  `16d1513eb`. Its import commit is not an integration commit.
- Harness commits: `17e70e484` and `f2e420f87`. The latter adds paired cycle
  durations and validates each node interval against its matching cycle, with
  clock-conversion tolerance equal to the larger of 1% or 1 microsecond.

## Ordinary updates

Times are **microseconds per evaluation**, over 100,000 source rows. Each row
uses 303 update samples. Node intervals and complete cycles come from separate
runs. The [node CSV](results/2026-11-04/group-by-nodes.csv) also contains
complete-cycle durations from the profiling run; every node interval passed
the same-cycle containment check. The
[cycle CSV](results/2026-11-04/group-by.csv) contains the unprofiled results.

| Workload | Changed source rows | Groups | Sums | Node median | Node p95 | Cycle median | Cycle p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Value update | 1 | 100 | 1 | 9.97 | 12.05 | 12.00 | 21.32 |
| Value update | 100 | 100 | 1 | 29.09 | 30.28 | 37.48 | 53.80 |
| Value update | 10,000 | 100 | 1 | 596.24 | 1,010.63 | 1,162.54 | 1,937.59 |
| Value update | 1 | 10,000 | 1 | 10.38 | 18.90 | 12.54 | 15.40 |
| Value update | 100 | 10,000 | 1 | 29.78 | 44.09 | 38.07 | 73.13 |
| Value update | 10,000 | 10,000 | 1 | 1,936.25 | 2,162.34 | 2,665.93 | 4,172.96 |
| Key move | 100 | 100 | 1 | 34.48 | 36.54 | 43.31 | 84.00 |
| Remove 100 / add 100 | 100 + 100 | 100 | 1 | 47.96 | 89.40 | 184.36 | 268.58 |
| Value update | 1 | 100 | 2 | 15.21 | 16.13 | 17.92 | 32.49 |
| Scalar parameter update | 0 | 100 | 1 | 3,048.46 | 4,264.84 | 3,075.29 | 3,326.41 |

The scalar update recalculates contributions for all 100,000 rows. All other
single-sum cases use `i64`. The two-sum case affects both its integer and double
aggregations. Remove/add changes 100 source positions by removing their old
rows and adding new rows with new user keys.

First evaluation medians range from 7.8 to 13.5 milliseconds across these
cases. Source rows are already populated; these intervals include group and
contribution-state construction. Each initial statistic has only three samples.

The first complete-cycle run has similar medians, but the first profiling run
contains substantially higher results for several cases. That profiling run
did not record paired cycle durations, so its cause cannot be established.
Both original files are retained:
[cycles](results/2026-11-04/group-by-run1.csv) and
[nodes](results/2026-11-04/group-by-nodes-run1.csv). The table uses the repeated
runs with same-cycle profile validation. Timing variation remains visible in
the p95 values.

## Interpretation

Small value updates have similar node cost with 100 and 10,000 groups. Ordinary
updates use cached membership and contributions rather than reading all keys.
Large updates become more expensive when they affect more output groups: the
10,000-row cases publish 100 versus 10,000 group results.

For remove/add churn, the node median is 48 microseconds within a profiled cycle
median of 177 microseconds. Source mutation, publication, and cleanup account
for substantial additional graph work. Scalar-parameter changes process every
current row and take about 3 milliseconds in the node itself.

These workloads exercise native batches, integer grouping keys, numeric sums,
and stable group membership counts. String keys, nullable data, reference
switches, and group creation/removal have separate correctness coverage; their
costs require additional workloads.

## Overflow recovery

Times are milliseconds for the complete result sweep across all groups.
The before run uses the source before `384de57e7`; the after run includes its
compensated arithmetic and shared recovery scan. Both use the same harness.
[Before CSV](results/2026-11-04/group-by-overflow-before.csv),
[after CSV](results/2026-11-04/group-by-overflow-after.csv).

| Groups | Rows | Before median | Before p95 | After median | After p95 | Median ratio |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 64 | 0.009391 | 0.009451 | 0.001600 | 0.001740 | 5.9× |
| 256 | 512 | 0.615957 | 0.650120 | 0.012702 | 0.013132 | 48.5× |
| 2,048 | 4,096 | 37.000243 | 41.398657 | 0.097429 | 0.100890 | 379.8× |

The old result sweep scanned all source rows once per affected group. Shared
recovery removes that multiplication: increasing groups and rows eightfold
increases the new median about eightfold. Recovery still traverses all current
rows for each aggregation that needs it. These measurements deliberately make
every group overflow; they do not represent ordinary finite updates.

## Run

Configure a Release frame build with `HG_FRAME_BUILD_BENCHMARKS=ON`, then run:

```sh
cmake --build <release-build> --target hg_frame_group_by_benchmark -j12
<release-build>/hg_frame_group_by_benchmark
<release-build>/hg_frame_group_by_benchmark --profile-nodes
<release-build>/hg_frame_group_by_benchmark --overflow-recovery
```
