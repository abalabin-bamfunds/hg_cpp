# Benchmarks

These benchmarks help estimate the cost of maintaining a graph as inputs change,
handling queued events, and processing tables or network messages. Each workload
measures a particular execution path; choose the one closest to your application.

These measurements were recorded between **17 October and 4 November 2026**.
Each section identifies the source revisions and environments used.
Lower times and memory use are better; higher throughput is better.
The median is the middle observation, and p95 or p99 describes the upper tail of
the measured samples. Observed latencies do not establish a maximum delivery time.

| Application need | Relevant measurements |
| --- | --- |
| Many independent calculations with sparse changes | [Mapped graphs](#mapped-graphs) |
| One aggregate across a changing dictionary | [Reduction](#reduction) |
| External events and real-time delivery | [Queues](#queues) |
| Running totals and grouped tables | [Frame aggregation](#frame-aggregation) |
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

The comparison from **17 October 2026** measures one run per configuration.
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
single runs under uncontrolled host contention; repeat runs would be needed to
estimate run-to-run variation.

## Reduction

When a single result combines many changing dictionary items, `reduce` maintains
that result by updating the affected parts of a reduction tree. This benchmark
sums a native `TSD<i64, TS<i64>>` through a reducer graph containing one addition
node. See [custom aggregation](guide/graphs/aggregation.md#custom-aggregation) for
the authoring pattern.

Each run populates the dictionary, then measures single-item updates, updates to
1% of the items, and removal and restoration of 1% of the items. Update times cover
the complete graph evaluation, including source mutation, reduction, result
validation, publication, and cleanup.

### Updates and membership changes

Times are **microseconds per evaluation**. A 1% change affects 100, 1,000, or
10,000 items at the respective dictionary sizes.

| Items | Path | One-item update | 1% update | 1% removal | 1% restoration |
| --- | --- | ---: | ---: | ---: | ---: |
| 10,000 | Reduce | 7.23 | 45.19 | 23.56 | 68.80 |
| 10,000 | Source only | 0.68 | 11.16 | 8.01 | 20.63 |
| 100,000 | Reduce | 16.57 | 446.97 | 210.57 | 641.98 |
| 100,000 | Source only | 3.45 | 182.27 | 77.46 | 193.68 |
| 1,000,000 | Reduce | 95.20 | 4,649.72 | 2,037.53 | 6,592.58 |
| 1,000,000 | Source only | 27.82 | 1,060.56 | 784.74 | 1,973.19 |

The source-only control applies the same changes to the same dictionary type
without a reduction or result observer. Its timings show the input-handling cost;
subtracting the two medians does not isolate reducer execution.

### Initial population and memory

These figures include the source dictionary and reduction. Population time covers
the first evaluation; the RSS increase is resident memory after population minus
resident memory with the empty runtime.

| Items | Population, ms | Populated RSS increase, MiB |
| --- | ---: | ---: |
| 10,000 | 10.47 | 1.72 |
| 100,000 | 78.34 | 15.18 |
| 1,000,000 | 733.02 | 149.32 |

RSS includes allocator effects and does not isolate reducer storage. It is sampled
before update cycles, so later memory peaks are excluded.

### Measurement conditions

The **4 November 2026** run used engine and harness `a2b97f570`, GCC 14.3.0 Release
without LTO, and an AMD EPYC 7R13 Linux KVM VM with affinity to CPU 31. Three fresh
processes per size and path each measured 100 single-item updates, 100 bursts, and
100 remove/restore pairs, with no discarded warmup cycles. Update figures are
medians of three process medians; population and RSS figures are medians across
the three reduction processes. Every reduction process matched all 401 expected
aggregate outputs. Source-only processes checked source completion.

Host contention was uncontrolled. At 10,000 items, the three single-item reduction
medians ranged from 4.62 to 9.13 µs; at 1,000,000 items, they ranged from 93.11 to
95.22 µs. These measurements cover valid native scalar items with a one-node
reducer; references, validity changes, and larger reducer graphs need separate
measurements.

## Queues

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
10,000-group bulk update and full recalculation.

These are separate runs without affinity, with substantial timing variation.
Profiling adds overhead: for the one-row, 100-group case the unprofiled HG cycle
median is 0.012 ms, versus 0.029 ms in the profiling run. Node intervals include
preceding publication and scheduling between profiler checkpoints, and end before
the node's output publication. The measurements do not isolate profiling overhead
from run-to-run variation. Arrow result conversion back to a native frame is excluded.

## Web extension

For applications using [HTTP](extensions/web.md) or [WebSocket](extensions/websocket.md)
services, the loopback benchmark measures complete native client/server graph
exchanges, including graph services, queues, transport threads, payload generation,
and validation. It does not measure an external network or maximum server capacity.

These **4 November 2026** measurements used GCC 14.3 Release on an AMD EPYC 7R13
Linux KVM VM with process affinity to CPUs 0–3. Loaded dependencies were libcurl
8.20.0 and OpenSSL 3.6.2. Each case drained 128 warmup exchanges before timing.
Throughput is the median sample rate; RTT percentiles pool exchanges across samples.

RTT starts before request construction and ends at the reply observer. Wiring,
startup, initial connections, and teardown are excluded. Connections opened during
measurement are included.

### HTTP

Each case uses 64-byte bodies and five samples of 4,096 exchanges. Concurrency
uses separate clients with one outstanding request each.

| Protocol | Outstanding exchanges | Exchanges/s | RTT p50, µs | RTT p99, µs |
| --- | ---: | ---: | ---: | ---: |
| HTTP | 1 | 2,675 | 358.3 | 891.5 |
| HTTPS | 1 | 2,304 | 419.4 | 712.0 |
| HTTP | 16 | 10,529 | 1,360.9 | 4,029.0 |
| HTTPS | 16 | 282 | 64,699.0 | 125,494.9 |

The engine was `be009f7b`, with a cache of eight idle HTTP connections. At
16 clients, connection churn contributes to the measured workload. Host contention
varied during the sweep, and four messages about closed response queues occurred
at shutdown after reply validation.

### WebSocket

These measurements use `TCP_NODELAY` on both the client socket and the accepted
server socket. The window is the maximum number of outstanding messages on one connection.

| Body | Window | Protocol | Messages/s | RTT p50, ms | RTT p99, ms |
| --- | ---: | --- | ---: | ---: | ---: |
| 64 KiB | 1 | WebSocket | 758 | 1.090 | 3.491 |
| 64 KiB | 1 | Secure WebSocket | 743 | 1.195 | 3.353 |
| 64 B | 64 | WebSocket | 23,988 | 2.258 | 6.320 |
| 64 B | 64 | Secure WebSocket | 21,280 | 2.631 | 6.596 |
| 64 KiB | 64 | WebSocket | 1,834 | 34.001 | 53.094 |
| 64 KiB | 64 | Secure WebSocket | 1,671 | 41.030 | 121.858 |

The measured build is based on `0c3ad173` with the socket options enabled. Each
case uses five samples: 64 messages per sample for window 1, and 4,096 for window
64. Host load was high after these runs, and CPU pressure was not sampled for each
case, so the window-64 rates should be interpreted with that variation in mind.

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
For reduction, these commands measure 100 single-item updates, 100 bursts, and
100 remove/restore pairs at 100,000 items. Repeat with `--size 10000` and
`--size 1000000` for the other sizes above:

```sh
cmake --build --preset benchmark --target hg_reduce_benchmark
build/benchmark/benchmarks/hg_reduce_benchmark --size 100000 --samples 100 --lifecycle-rounds 100
build/benchmark/benchmarks/hg_reduce_benchmark --size 100000 --mode source --samples 100 --lifecycle-rounds 100
```

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

Replacing `--quick` with `--messages 4096 --samples 5` runs the full default matrix.
Benchmark executable paths follow the build generator; multi-configuration generators
also need `--config Release`.
