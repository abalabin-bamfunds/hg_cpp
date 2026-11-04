# Benchmarks

These benchmarks help estimate the cost of maintaining a graph as inputs change,
handling queued events, and processing tables or network messages. Each workload
measures a particular execution path; choose the one closest to your application.

This page collects measurements recorded through **4 November 2026**. They describe
the revisions and environments stated below. They have not been rerun for this
documentation update. Lower times and memory use are better; higher throughput is better.
The median is the middle observation, and p95 or p99 describes the upper tail of
the measured samples. Observed latencies do not establish a maximum delivery time.

| Application need | Relevant measurements |
| --- | --- |
| Many independent calculations with sparse changes | [Mapped graphs](#mapped-graphs) |
| External events and real-time delivery | [Queues and scheduling](#queues-and-scheduling) |
| Running totals and grouped tables | [Frame aggregation](#frame-aggregation) |
| Native columns consumed by Arrow | [Frame traversal and export](#frame-traversal-and-export) |
| HTTP or WebSocket graph services | [Web extension](#web-extension) |

## Mapped graphs

When a collection has many items but only a few change at once, `map` maintains a
separate inner graph for each item. This workload measures sparse updates, branch
removal and restoration, and changes to a shared input that activate every branch.
See [iteration](guide/graphs/iteration.md) for the authoring pattern.

The graph contains two 80,000-branch maps in series and a nested map with 10,000
parents and eight children per parent: **250,000 active inner graphs** in total.
Each arithmetic operation is a separate node. Ordinary cycles change 250 flat
items and 250 children. The 10,000 measured cycles also include 400 lifecycle
cycles and ten full broadcasts. All eight configurations produced the expected
6,705,000 output events and matching checksums.

### Python and C++ execution paths

The historical comparison from **17 October 2026** reports one run per configuration.
Cycle totals exclude workload generation, wiring, and initial population.

| Engine and arithmetic implementation | Population, s | 10,000 cycles, s | Ordinary p50, ms | Graph RSS, GB |
| --- | ---: | ---: | ---: | ---: |
| HG, C++ nodes | 0.888 | 14.606 | 1.095 | 0.222 |
| HG, Python graph with library operators | 1.187 | 84.788 | 6.213 | 0.286 |
| HG, Python nodes | 1.794 | 107.728 | 8.158 | 0.296 |
| hgraph, C++ nodes | 4.231 | 51.510 | 4.162 | 1.498 |
| hgraph, Python graph with library operators | 6.723 | 122.748 | 9.892 | 1.663 |
| hgraph, Python nodes | 11.048 | 165.223 | 13.070 | 3.329 |
| hgraph 0.5.42, C++ engine with Python nodes | 13.388 | 146.289 | 10.846 | 4.577 |
| hgraph 0.5.42, Python engine with Python nodes | 330.706 | 868.846 | 56.111 | 7.291 |

All runs used an AMD EPYC 7R13 Linux host, CPU affinity to CPU 31, and a 12 GiB
process address-space limit. Native builds used GCC 14.3 Release; Python runs used
CPython 3.13 for HG and 3.12 for hgraph. HG native used `36eb21fb`, HG Python
`1c9ad02d`, hgraph `de44e25b`, and hgraph 0.5.42 `c34dba1d`.

Graph RSS is process resident memory before graph stop minus resident memory before
wiring, expressed in decimal GB. It can include process allocations beyond graph
storage. Some memory samples came from matching reruns. Python adapters use
different timing boundaries, so precise cross-engine ratios need care. These are
single runs under uncontrolled host contention, and the original raw JSON files
are no longer available. The {download}`historical report <../../benchmarks/results/high_count_map/clean_250k_10k_2026-10-17.md>`
retains lifecycle timings, source attribution, and measurement details.

### Runs with retained raw measurements

Separate native HG runs retain their full JSON records. These use the same
250,000-graph, 10,000-cycle workload, but differ from the historical comparison above.
Each record includes a validated checksum, latency classes, and memory samples.

| HG revision and raw JSON | CPU affinity | Population, s | 10,000 cycles, s | Ordinary p50, ms |
| --- | ---: | ---: | ---: | ---: |
| {download}`067dc7a5 <../../benchmarks/results/high_count_map/hg/067dc7a5cd754b9adae9d1cbf106b7848ca520f3.json>` | 47 | 0.789 | 18.085 | 1.142 |
| {download}`6b7bb435 <../../benchmarks/results/high_count_map/hg/6b7bb4351022dc7afce108ad735fb87083864c5a.json>` | 47 | 0.897 | 15.418 | 1.193 |
| {download}`e99c8c16 <../../benchmarks/results/high_count_map/hg/e99c8c165634878addec1bcd07259938588f536d.json>` | 47 | 0.875 | 14.878 | 1.145 |
| {download}`bcc9b1e7 <../../benchmarks/results/high_count_map/hg/bcc9b1e7f7b57a592f022ba02c6fc82b22d621c7.json>` | 0 | 0.779 | 13.304 | 1.033 |

These runs used GCC 14.3 Release on AMD EPYC 7R13 Linux hosts. The last run has a
different CPU configuration; this table alone does not isolate the effect of code
changes. Additional raw comparison records cover
{download}`hgraph native C++ at 7c55fcf1 <../../benchmarks/results/high_count_map/hgraph-current/7c55fcf19bb2fce0ae3a5292eab2855376e084fa.json>`
and hgraph 0.5.42 at `c34dba1d` with its
{download}`C++ engine <../../benchmarks/results/high_count_map/hgraph-0.5/c34dba1db8a1c873ee303796db4c8fc48c8cc5be-cpp.json>`
and {download}`Python engine <../../benchmarks/results/high_count_map/hgraph-0.5/c34dba1db8a1c873ee303796db4c8fc48c8cc5be-python.json>`.
Their adapter revisions and phase boundaries are recorded in each file.

## Queues and scheduling

Queues connect external producers to graph inputs. The queue benchmarks distinguish
the cost of writing messages, evaluating queued messages, and waking a waiting
engine. See [external event sources](reference/sources.md) for the queue API.

### Queue throughput

The **21 October 2026** baseline used native `i64` messages, with 100,000 messages
per sample and 21 samples per case. Rates below are medians across samples.

| Operation | ns per input message | Million input messages/s |
| --- | ---: | ---: |
| Write, one producer | 346.3 | 2.89 |
| Write, four producers | 303.4 | 3.30 |
| Drain, standard queue | 623.4 | 1.60 |
| Drain, eliding queue | 237.9 | 4.20 |
| Drain, batch queue | 388.9 | 2.57 |

Write timing uses prebuilt literals with no concurrent reader or engine wakeup.
Drain timing starts with a preloaded queue. Standard drain emits 100,000 scalar
ticks, eliding drain emits one scalar tick, and batch drain emits one list tick
and scans its contents. These policies deliver different outputs; the numbers
are separate write and drain costs, rather than end-to-end throughput.

### Real-time delivery

Delivery timing starts before literal construction and queue write, and ends in
a downstream observer. It includes engine wakeup and the observer mutex. Each
exchange has one message in flight and a 1 ms idle gap outside the timed interval.

| Path | p50, µs | p95, µs | p99, µs |
| --- | ---: | ---: | ---: |
| Queue write to downstream observation | 18.151 | 33.262 | 42.462 |
| Plain semaphore control | 15.461 | 34.242 | 38.782 |
| Engine wakeup control | 13.890 | 21.761 | 32.532 |

Queue percentiles pool 21,000 messages; each control pools 3,000 notifications.
Controls ran sequentially with varying thread placement, so their differences
do not establish a ranking between wakeup mechanisms. Queue delivery's observed
maximum was 1.217 ms.

The baseline used engine `08302061`, harness/corrections `10481b3a` and `1ca7da5c`,
GCC 14.3 Release without LTO, and an AMD EPYC 7R13 Linux KVM VM. Queue and wakeup
processes shared CPUs 0–3; individual threads were not pinned.
{download}`Raw baseline results <../../benchmarks/results/queue_realtime/1ca7da5c5ad1b6a5e28aa84be4e35976952bba7d-rerun.json>`
include samples and environment counters. An earlier contention-affected sweep
is retained separately in the repository.

### Scheduling and periodic callbacks

In the same baseline, scheduling one of 32 nodes took **77.2 ns per cycle**;
scheduling all 32 took **963.6 ns per cycle**. Each is the median of three
200,000-cycle runs pinned to CPU 0.

A separate paired experiment compares periodic callback polling before and after
the eight-cycle cadence was implemented. Each version has 21 pairs of
1,000,000-cycle simulation runs. Timing includes wiring, start, and stop.

| Callback setting | Per-cycle polling, ns/cycle | Eight-cycle polling, ns/cycle |
| --- | ---: | ---: |
| Disabled | 113.066 | 112.330 |
| Enabled, 50 ms interval | 145.581 | 117.482 |
| Median paired enabled-minus-disabled cost | 32.395 | 5.123 |

This comparison uses engine definitions from `06f12da7` and `9473d53d`, with
GCC 14.3 Release and CPU affinity 0 on the same VM. Sample ranges are wide,
including negative paired differences after the change. The medians describe
this timer workload. The cadence can delay a due callback by seven additional
complete cycles. {download}`Raw callback comparison <../../benchmarks/results/periodic_callback_overhead/9473d53d89440ff11d9b6aca49f8cf62bdbb07d2-timer-ab.json>`.

## Frame aggregation

If only a few table cells change, retaining contributions between evaluations
can avoid summing every row again. These measurements cover native integer
columns and grouped sums; see [frame grouping](extensions/frame-grouping.md).

### Incremental column sums

Each cycle changes one row. Both implementations produce the same result:
the incremental sum retains prior contributions, while the full-scan node reads
every active cell. Times below cover complete graph evaluations, including source
mutation, filtering where applicable, output publication, and cleanup.

| Input | Source rows | Active rows | Incremental median, ms | Full-scan median, ms |
| --- | ---: | ---: | ---: | ---: |
| Column | 1,000 | 1,000 | 0.000770 | 0.002370 |
| Column | 10,000 | 10,000 | 0.000990 | 0.019970 |
| Column | 100,000 | 100,000 | 0.003020 | 0.198174 |
| Filtered column | 100,000 | 50,000 | 0.007890 | 0.588571 |

The **27 October 2026** run used GCC 14.3 Release, an AMD EPYC 7R13 VM, and
incremental-sum changes on base `5169a8d5`. Each case combines 303 measured cycles
from three graph instances after ten warmup cycles each. Initialization and
destruction are excluded; every result is checked outside timing. Bulk changes,
rebinding, and overflow recovery require different workloads.
{download}`Complete-cycle CSV <../../extensions/frame/benchmarks/results/2026-10-27/incremental-sum.csv>`
and {download}`separate sum-node profiling CSV, 28 October <../../extensions/frame/benchmarks/results/2026-10-28/incremental-sum-nodes.csv>`.

### Grouped sums

The **4 November 2026** native benchmark groups 100,000 rows and maintains one
integer sum per group. Complete-cycle medians below include source mutation,
aggregation, publication, the result-size sink, and cleanup.

| Update | Changed rows | Groups | Median, µs | p95, µs |
| --- | ---: | ---: | ---: | ---: |
| Value | 1 | 100 | 12.00 | 21.32 |
| Value | 100 | 100 | 37.48 | 53.80 |
| Value | 10,000 | 100 | 1,162.54 | 1,937.59 |
| Value | 1 | 10,000 | 12.54 | 15.40 |
| Value | 10,000 | 10,000 | 2,665.93 | 4,172.96 |
| Group key move | 100 | 100 | 43.31 | 84.00 |
| Remove and add | 100 + 100 | 100 | 184.36 | 268.58 |
| Captured scalar parameter | All contributions | 100 | 3,075.29 | 3,326.41 |

Each case has 303 update samples after warmup; an untimed replay checks final
totals against full recomputation. Initial evaluations are measured separately.
Small updates benefit from retained membership and contributions. Changing the
captured scalar parameter recalculates contributions for every source row.

The build used GCC 14.3 Release on an AMD EPYC 7R13 VM without CPU affinity.
Frame source was based on `29e6b8f43`, with grouped-aggregation and numerical
changes through `384de57e7`; harness commits were `17e70e484` and `f2e420f87`.
The {download}`native report <../../extensions/frame/benchmarks/group_by.md>`
records the complete dependency configuration.
{download}`Cycle CSV <../../extensions/frame/benchmarks/results/2026-11-04/group-by.csv>`
and {download}`separate node profiling CSV <../../extensions/frame/benchmarks/results/2026-11-04/group-by-nodes.csv>`.

### Comparison with Arrow Compute

A fresh comparison runs the same update schedule through HG and Arrow 24 Acero
with threading disabled. HG retains aggregation state; Arrow computes a new full
result table each time. This shows where incremental maintenance helps and where
batch recomputation is cheaper.

Times are **milliseconds per update**. HG measures the profiled aggregation-node
interval. Arrow includes input preparation, query execution, and destruction.
Both exclude source mutation and sink evaluation.

| Update | Changed rows | Groups | HG node | Arrow borrowed | Arrow dense copy |
| --- | ---: | ---: | ---: | ---: | ---: |
| Value | 1 | 100 | 0.024 | 1.456 | 2.400 |
| Value | 100 | 100 | 0.058 | 1.461 | 2.409 |
| Value | 10,000 | 100 | 0.915 | 1.543 | 2.427 |
| Value | 10,000 | 10,000 | 3.771 | 2.381 | 3.174 |
| Captured scalar parameter | All contributions | 100 | 5.472 | 1.804 | 2.760 |

Each median uses 303 samples. The comparison uses the native configuration above
and comparator `77b109bcf`. Borrowed inputs retain 15 chunks per column; dense
inputs copy each column into one owned chunk. Arrow is faster for the shown
10,000-group bulk update and full recalculation. The raw files also retain initial
evaluations, where HG constructs aggregation state and reports a complete-cycle interval.

These are separate runs without affinity, with substantial timing variation.
Profiling adds overhead: for the one-row, 100-group case the unprofiled HG cycle
median is 0.012 ms, versus 0.029 ms in the profiling run. Node intervals include
preceding publication and scheduling between profiler checkpoints, and end before
the node's output publication. The measurements do not isolate profiling overhead
from run-to-run variation. Arrow result conversion back to a native frame is excluded.

{download}`Arrow phase CSV <../../extensions/frame/benchmarks/results/2026-11-04/group-by-arrow.csv>`,
{download}`HG comparison node CSV <../../extensions/frame/benchmarks/results/2026-11-04/group-by-comparison-bg-nodes.csv>`,
and {download}`HG comparison cycle CSV <../../extensions/frame/benchmarks/results/2026-11-04/group-by-comparison-bg.csv>`.

## Frame traversal and export

When another library consumes frame columns, row layout affects the cost of
traversal and export. Borrowed Arrow export retains native buffers; dense gather
copies selected values into contiguous buffers. Fragmented selections can make
the borrowed path create many small Arrow chunks.

The **23 October 2026** bounded-validity-search comparison measures frame revisions
`2f180034` and `3b59d9d7`, against HG `5d48e914`. Each value is the median of three
process medians, with two warmup and eleven measured cycles per process. Borrowed
consumer timing includes export, Arrow sum, and destruction.

| Workload | Consumer | Before, ms | After, ms |
| --- | --- | ---: | ---: |
| 100,000 dense rows | Borrowed Arrow + sum | 0.220 | 0.032 |
| Clustered selection and deletion | Borrowed Arrow + sum | 0.180 | 0.081 |
| Scattered selection, clustered deletion | Borrowed Arrow + sum | 16.530 | 16.444 |
| 100,000 dense rows | Native batch sum | 0.085 | 0.086 |

These all-valid workloads used GCC 14.3 Release and Arrow 24 on an AMD EPYC 7R13
Linux VM without affinity. Scattered selection retained 49,168 Arrow chunks, so
faster validity search did little for that case.
{download}`Full comparison CSV <../../extensions/frame/benchmarks/results/2026-10-23/bounded-search/comparison.csv>`.

Compaction can reduce fragmentation before repeated consumers read the same
table. In the **24 October 2026** range-relocation run at `d8dbd8c3`, eight-column
frames started with 32,768 rows. For 5% scattered deletion, one borrowed-Arrow
consumer took 3.999 ms before compaction. The modeled compaction-plus-consumer
total was 0.915 ms; dense gather plus sum took 1.497 ms. These totals add medians
of separately timed phases and are not direct end-to-end measurements. At 0.5%
deletion, compaction did not repay its cost for one Arrow consumer.
The run used GCC 14.3 Release, Arrow 24, and an AMD EPYC 7R13 Linux VM, with five
samples per case. {download}`Compaction CSV <../../extensions/frame/benchmarks/results/2026-10-24/compaction-range/after.csv>`.

## Web extension

For applications using [HTTP](extensions/web.md) or [WebSocket](extensions/websocket.md)
services, the loopback benchmark measures complete native client/server graph
exchanges, including graph services, queues, transport threads, payload generation,
and validation. It does not measure an external network or maximum server capacity.

### HTTP and WebSocket baseline

The **4 November 2026** baseline uses 64-byte bodies, five samples of 4,096
exchanges, and 128 fully drained warmup exchanges per case. Throughput is the
median sample rate; RTT percentiles pool all 20,480 exchanges per case.

| Protocol | Outstanding exchanges | Exchanges/s | RTT p50, µs | RTT p99, µs |
| --- | ---: | ---: | ---: | ---: |
| HTTP | 1 | 2,675 | 358.3 | 891.5 |
| HTTPS | 1 | 2,304 | 419.4 | 712.0 |
| HTTP | 16 | 10,529 | 1,360.9 | 4,029.0 |
| HTTPS | 16 | 282 | 64,699.0 | 125,494.9 |
| WebSocket | 1 | 4,968 | 192.9 | 442.3 |
| Secure WebSocket | 1 | 3,839 | 227.2 | 566.6 |
| WebSocket | 64 | 21,665 | 2,245.0 | 6,685.9 |
| Secure WebSocket | 64 | 16,631 | 2,633.2 | 10,479.1 |

HTTP concurrency uses separate clients with one outstanding request each;
WebSocket concurrency uses a window on one connection. RTT starts before request
construction and ends at the reply observer. Wiring, startup, initial connections,
and teardown are excluded. Connections opened during measurement are included.

The engine was `be009f7b`, built with GCC 14.3 Release on an AMD EPYC 7R13 Linux
KVM VM with process affinity to CPUs 0–3. Loaded dependencies were libcurl 8.20.0
and OpenSSL 3.6.2. Host contention varied during the sweep, and four messages about
closed response queues occurred at shutdown after reply validation.
{download}`Raw baseline JSONL, including 4 KiB cases and HTTP controls <../../benchmarks/results/web_extension/2026-11-04/release-full.jsonl>`.

### Connection and large-message follow-ups

Connection retention matters at higher HTTP concurrency. A follow-up at `a2f3d723`
with 16 clients compared the normal cache of eight idle connections with an
experimental cache of 64. For 64-byte HTTPS bodies, median throughput increased
from 326 to 9,492 requests/s, while pooled p99 fell from 79.42 to 4.51 ms. Each
configuration used five samples of 4,096 requests. The cache-64 variant was an
experiment and was restored to eight; this is not a measured default configuration.
{download}`Cache-8 JSONL <../../benchmarks/results/web_extension/2026-11-04/followup/cache8-http-https-window16.jsonl>`
and {download}`cache-64 JSONL <../../benchmarks/results/web_extension/2026-11-04/followup/cache64-http-https-window16.jsonl>`.

The WebSocket follow-up enabled `TCP_NODELAY` on both client and accepted server
sockets. For 64 KiB messages with one outstanding exchange:

| Protocol | Earlier RTT p50, ms | Follow-up RTT p50, ms | Follow-up messages/s |
| --- | ---: | ---: | ---: |
| WebSocket | 117.810 | 1.090 | 758 |
| Secure WebSocket | 118.530 | 1.195 | 743 |

Each case uses five samples of 64 messages. The baseline is `a2f3d723`; the follow-up
tree is based on `0c3ad173` with the socket changes. This is a cross-revision
comparison and does not isolate the socket option's effect. Larger-window runs
had different host load and gave inconclusive throughput changes.
{download}`Earlier large-message JSONL <../../benchmarks/results/web_extension/2026-11-04/followup/ws-64k-window1-64x5.jsonl>`
and {download}`TCP_NODELAY JSONL <../../benchmarks/results/web_extension/2026-11-04/followup/nodelay-64k-window1-64x5.jsonl>`.

## Running benchmarks

Benchmark programs are built from a source checkout. Core builds need CMake 3.31,
Ninja, a C++23 compiler, ICU, and fmt; CMake fetches the remaining core dependencies.
Use Release builds and run measurements sequentially, keeping the machine and
dependency configuration consistent between comparisons.

```sh
cmake --preset benchmark
cmake --build --preset benchmark --target hg_high_count_map_benchmark hg_queue_realtime_benchmark
build/benchmark/benchmarks/hg_high_count_map_benchmark --verify-workload
build/benchmark/benchmarks/hg_high_count_map_benchmark --profile smoke --cycles 120
build/benchmark/benchmarks/hg_queue_realtime_benchmark --quick
```

The small runs validate the harnesses. The full map workload uses
`build/benchmark/benchmarks/hg_high_count_map_benchmark --profile 250k --cycles 10000`.
On Linux, `taskset -c <available-cpu>` can pin a run. The HG Python
adapter requires a matching Release Python extension and accepts the same profile
and cycle arguments:

```sh
python bindings/python/benchmarks/high_count_map.py --profile smoke --cycles 120
```

Frame benchmarks use the separate `extensions/frame` CMake project with a matching
HG installation on `CMAKE_PREFIX_PATH` and `HG_FRAME_BUILD_BENCHMARKS=ON`.
Arrow workloads also require `HG_FRAME_BUILD_ARROW=ON` and the Arrow, ArrowCompute,
and ArrowAcero CMake packages. For example:

```sh
cmake -S extensions/frame -B build/frame-benchmark -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/hg-install -DBUILD_TESTING=OFF \
  -DHG_FRAME_BUILD_BENCHMARKS=ON
cmake --build build/frame-benchmark --target hg_frame_group_by_benchmark
build/frame-benchmark/hg_frame_group_by_benchmark
```

The web target needs `HG_BUILD_WEB=ON` as well as `HG_BUILD_BENCHMARKS=ON`, with
libcurl and OpenSSL available. It uses loopback listeners and test TLS material.
This command validates the protocol matrix and bypasses environment proxies for
loopback addresses:

```sh
cmake --preset benchmark -DHG_BUILD_WEB=ON
cmake --build --preset benchmark --target hg_web_extension_benchmark
env NO_PROXY=localhost,127.0.0.1,::1 no_proxy=localhost,127.0.0.1,::1 \
  build/benchmark/extensions/web/hg_web_extension_benchmark --quick
```

Replacing `--quick` with `--messages 4096 --samples 5 --output results.jsonl`
runs the recorded baseline matrix. Benchmark executable paths follow the build
generator; multi-configuration generators also need `--config Release`.

## Other benchmark coverage

Smaller workloads help diagnose a particular cost. Their reports preserve the
build configuration, source attribution, and measurement limits.

| Recorded comparison | Coverage and evidence |
| --- | --- |
| Evaluation contexts | Child construction, context sizes, and flat/nested evaluation. The {download}`context-layout report <../../benchmarks/evaluation_context.md>` does not establish an overall evaluation improvement. |
| Invocation-end callbacks | Flat, scalar-output, nested, and scheduling costs. The {download}`callback report <../../benchmarks/results/invocation_end_callbacks/README.md>` includes raw samples; scheduling results are preliminary. |
| Isolated polling | Clock reads and polling strides outside graph evaluation. {download}`Raw polling samples <../../benchmarks/results/periodic_callback_overhead/gcc-14.3-linux-x86_64-2026-10-21.txt>` accompany the paired graph comparison above. |
| HTTP header conversion | ASCII and Latin-1 conversion, including output equivalence checks. {download}`Conversion report, 29 October <../../benchmarks/http_header_bench.md>`. |
| Frame expressions | Payload/predicate/scalar updates and row membership changes. {download}`22 October cycle CSV <../../extensions/frame/benchmarks/results/2026-10-22/expression-release-arrow-off.csv>` and {download}`23 October optimization comparison <../../extensions/frame/benchmarks/results/2026-10-23/comparison.csv>`. |
| Compaction storage | Constructed-cell bitmap comparison. {download}`Before CSV <../../extensions/frame/benchmarks/compaction_constructed_cells_before.csv>` and {download}`after CSV <../../extensions/frame/benchmarks/compaction_constructed_cells_after.csv>`; timing changes are not causally isolated. |
| Grouped-sum overflow recovery | Deliberately overflowing every group; only the result sweep is timed. {download}`Before CSV <../../extensions/frame/benchmarks/results/2026-11-04/group-by-overflow-before.csv>` and {download}`after CSV <../../extensions/frame/benchmarks/results/2026-11-04/group-by-overflow-after.csv>`. |

Runnable suites without retained timing results cover tree reduction, custom versus
standard hash dictionaries, scalar dictionary lookup and cursor traversal, Python
dictionary views versus eager conversion, node input views and list access,
selected dictionary references, membership notifications, and regular expressions.
Core sources are under `benchmarks/`; Python sources are under
`bindings/python/benchmarks/`. Build targets are listed in `benchmarks/CMakeLists.txt`.

Archived frame foundation, schema/storage, and Arrow probe experiments remain in
`extensions/frame/archive/`. The earliest Arrow report mixed Release extension
code with a Debug HG library, so it should be read separately from the later
matching-Release measurements.
