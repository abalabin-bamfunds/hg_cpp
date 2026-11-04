# Benchmark catalog

The [published benchmark page](../user/benchmarks.md) collects application-facing
results and their limits. This catalog identifies every benchmark harness and
retained result family as of 4 November 2026. Paths below are relative to the
repository root unless a table states otherwise. Raw records remain authoritative;
the public page summarizes application-facing workloads and their measurement conditions.

## Core and Python harnesses

The `benchmark` CMake preset enables `HG_BUILD_BENCHMARKS` and uses Release builds.
Native targets below use the prefix `hg_` and suffix `_benchmark`; for example,
`queue_realtime` builds `hg_queue_realtime_benchmark`. Sources are under `benchmarks/`.

| Target stem | Source | Workload | Retained results |
| --- | --- | --- | --- |
| `high_count_map` | [high_count_map_benchmark.cpp](../../benchmarks/high_count_map_benchmark.cpp) | Sparse, lifecycle, broadcast, serial maps, nested maps | [Workload and adapters](../../benchmarks/high_count_map.md), [raw records](../../benchmarks/results/high_count_map/README.md) |
| `reduce` | [reduce_benchmark.cpp](../../benchmarks/reduce_benchmark.cpp) | Tree reduction; sparse/burst/lifecycle updates; source-only control | [4 November measurements](../../benchmarks/results/reduce/2026-11-04/README.md) |
| `runtime_scheduling` | [runtime_scheduling_benchmark.cpp](../../benchmarks/runtime_scheduling_benchmark.cpp) | Dense/sparse scheduling and runtime storage | Queue baseline and invocation-end callback comparison below |
| `queue_realtime` | [queue_realtime_benchmark.cpp](../../benchmarks/queue_realtime_benchmark.cpp) | Writes, preloaded drains, delivery latency, timer callbacks | [Harness guide](../../benchmarks/queue_realtime.md), queue and polling results below |
| `evaluation_context` | [evaluation_context_benchmark.cpp](../../benchmarks/evaluation_context_benchmark.cpp) | Child contexts; flat, scalar-output, nested evaluation | [Context-layout report](../../benchmarks/evaluation_context.md), queue baseline, invocation-end comparison |
| `container` | [container_benchmark.cpp](../../benchmarks/container_benchmark.cpp) | Custom/standard hash dictionaries with sequential and mixed keys | No saved timing results |
| `scalar_dict` | [scalar_dict_benchmark.cpp](../../benchmarks/scalar_dict_benchmark.cpp) | Lookup and iteration at 32, 1,000, and 100,000 entries | [Harness guide](../../benchmarks/scalar_dict_benchmark.md); no saved timing results |
| `view` | [view_benchmark.cpp](../../benchmarks/view_benchmark.cpp) | Node input invocation; fixed/free TSL access | No saved timing results |
| `tsd_selected_reference` | [tsd_selected_reference_benchmark.cpp](../../benchmarks/tsd_selected_reference_benchmark.cpp) | Direct and representation-selected dictionary references | No saved timing results |
| `tsd_membership` | [tsd_membership_benchmark.cpp](../../benchmarks/tsd_membership_benchmark.cpp) | Root versus selected membership notification; shared hits; absent-key churn | No saved timing results |
| `string_regex` | [string_regex_benchmark.cpp](../../benchmarks/string_regex_benchmark.cpp) | Successful/failed matching with one/two capture groups | No saved timing results |

Additional programs:

| Source | Build or run | Coverage |
| --- | --- | --- |
| [Python high-count map](../../bindings/python/benchmarks/high_count_map.py) | Run with a matching Release Python extension | Python callbacks or library arithmetic on the portable workload |
| [Python dictionary mappings](../../bindings/python/benchmarks/dict_mapping.py) | Run with a matching Release Python extension | Lazy mappings, eager conversion, lookup, traversal, materialization, wrapper/buffer sizes; no saved timing results |
| [Wakeup control](../../benchmarks/wakeup_latency_control.cpp) | Standalone C++23, `-O3 -DNDEBUG -pthread -Iinclude` | Semaphore and engine wakeup controls; queue baseline results |
| [Polling experiment](../../benchmarks/periodic_callback_overhead_experiment.cpp) | Standalone C++23, `-O3 -DNDEBUG -pthread -Iinclude` | No-poll, per-cycle, strided, and coarse-clock controls |
| [HTTP header conversion](../../benchmarks/http_header_bench.cpp) | Standalone C++23, `-O3 -DNDEBUG` | Old/new ASCII and Latin-1 conversion; [29 October report](../../benchmarks/http_header_bench.md) |

## Graph and runtime result families

| Location | Measurements and interpretation |
| --- | --- |
| [High-count map JSON records](../../benchmarks/results/high_count_map/README.md) | Four native HG revisions, native hgraph `7c55fcf1`, and hgraph 0.5.42 Python/C++ engines at `c34dba1d`; one validated 250k/10k run per file. Environments and adapters differ; preserve each record's provenance. |
| [17 October eight-path comparison](../../benchmarks/results/high_count_map/clean_250k_10k_2026-10-17.md) | 250k inner graphs, 10k cycles, native/Python/library arithmetic. Raw JSON was held in temporary storage and is unavailable. Keep this historical report distinct from the commit-keyed records. |
| [Dictionary reduction](../../benchmarks/results/reduce/2026-11-04/README.md) | Engine/harness `a2b97f570`; native scalar reduction and source-only controls at 10k, 100k, and 1m items. Three processes per size/mode, with 100 cycles per update kind in each process; timing distributions, RSS, checksums, and environment retained. |
| [Queue and real-time baseline](../../benchmarks/results/queue_realtime/README.md) | Engine `08302061`; final `1ca7da5c…-rerun.json` includes queue, wakeup, and scheduling tables. Initial `10481b3a….json` retains a failed scheduling setup and contention-affected queue observations. |
| [Periodic callback comparison](../../benchmarks/results/queue_realtime/periodic_callback.md) | Matched per-cycle/eight-cycle polling experiment; [raw A/B JSON](../../benchmarks/results/periodic_callback_overhead/9473d53d89440ff11d9b6aca49f8cf62bdbb07d2-timer-ab.json) and [isolated polling samples](../../benchmarks/results/periodic_callback_overhead/gcc-14.3-linux-x86_64-2026-10-21.txt). |
| [Evaluation-context layout](../../benchmarks/evaluation_context.md) | Base `60a73309`, local/shared evaluation-time variants, construction variability, object sizes. Measurements do not establish improvement over the original context. |
| [Invocation-end callbacks](../../benchmarks/results/invocation_end_callbacks/README.md) | `fc3b80d1` versus `74ca1fa9`; context/evaluation and scheduling samples. Scheduling has two baseline runs and one candidate run, so its comparison is preliminary. |

The portable map workload is defined by
[C++ generation/checksums](../../benchmarks/high_count_map_workload.hpp) and its
[Python port](../../benchmarks/high_count_map_workload.py). External adapters are
[hgraph 0.5](../../benchmarks/hgraph_05_high_count_map/high_count_map.py),
[hgraph Python](../../benchmarks/hgraph_latest_high_count_map/run.py), and
[hgraph native](../../benchmarks/hgraph_current_high_count_map/native.cpp).
An adapter's source revision is part of the measurement: equivalent checksums
do not establish identical timing boundaries.

## Web extension

[web_extension_benchmark.cpp](../../benchmarks/web_extension_benchmark.cpp) builds
as `hg_web_extension_benchmark` when both `HG_BUILD_WEB` and `HG_BUILD_BENCHMARKS`
are enabled. It runs native HTTP/HTTPS and WS/WSS echo graphs plus raw HTTP
client controls. The web extension needs libcurl, OpenSSL, and Boost headers/URL;
CMake can fetch the required Boost components. The harness uses loopback sockets
and the extension's test TLS files.

Results are under `benchmarks/results/web_extension/2026-11-04/`:

| Report or files | Coverage |
| --- | --- |
| [Baseline report](../../benchmarks/results/web_extension/2026-11-04/README.md) | `release-full.jsonl`: 90 samples, 368,640 exchanges; 64 B/4 KiB bodies, HTTP windows 1/16, WS windows 1/64, HTTP controls. Engine `be009f7b`. |
| [Follow-up report](../../benchmarks/results/web_extension/2026-11-04/followup/README.md) | Connection cache, larger windows, large bodies, and TCP_NODELAY; exact sample counts and limits are recorded per case. |
| `followup/cache{8,64}-http-https-window16.jsonl` | Matched cache settings at `a2f3d723`; HTTPS connection churn. Cache 64 was reverted after measurement. |
| `followup/cache{8,64}-http8192.jsonl` | HTTP-only recheck; host-load differences prevent attribution of the 64-byte rate difference. |
| `followup/ws-64-4k-window*.jsonl` | One-connection 64 B/4 KiB throughput at windows 1, 64, 256, and 1,024. |
| `followup/ws-64k-window*.jsonl` | 64 KiB messages at windows 1, 64, and 256; bounded sample sizes. |
| `followup/nodelay-*.jsonl` | TCP_NODELAY comparison at windows 1/64 and one-sample diagnostic probe. Follow-up source is based on `0c3ad173` with socket changes. |
| Environment, CPU/pressure snapshots, stderr/stdout, runtime-library records | Build/runtime identification, varying contention, interrupted attempts, validation, and shutdown diagnostics. Interrupted runs supply no accepted samples. |

## Frame harnesses

The separate `extensions/frame` CMake project uses a matching HG installation.
Set `HG_FRAME_BUILD_BENCHMARKS=ON`. Arrow targets also need
`HG_FRAME_BUILD_ARROW=ON` and Arrow, ArrowCompute, and ArrowAcero packages.
Target names have prefix `hg_frame_` and suffix `_benchmark`.

| Target stem | Source under `extensions/frame/benchmarks/` | Purpose |
| --- | --- | --- |
| `expression` | [expression_benchmark.cpp](../../extensions/frame/benchmarks/expression_benchmark.cpp) | Payload/predicate/scalar updates, membership changes, add/remove |
| `incremental_sum` | [incremental_sum_benchmark.cpp](../../extensions/frame/benchmarks/incremental_sum_benchmark.cpp) | Incremental versus full scan; direct/filtered columns; optional node profiling |
| `group_by` | [group_by_benchmark.cpp](../../extensions/frame/benchmarks/group_by_benchmark.cpp) | Grouped sums; key moves, value updates, add/remove, scalar captures; node profiling and overflow recovery |
| `group_by_arrow` | Same source with `HG_FRAME_GROUP_BY_ARROW_BENCHMARK` | Acero `hash_sum`, borrowed/dense inputs; single-threaded query |
| `arrow` | [arrow_benchmark.cpp](../../extensions/frame/benchmarks/arrow_benchmark.cpp) | Native, borrowed Arrow, dense gather, and plain Arrow controls |
| `compaction` | [compaction_benchmark.cpp](../../extensions/frame/benchmarks/compaction_benchmark.cpp) | Movement plans, relocation, allocations, traversal/export, repeated consumers, computed columns |

## Frame result families

Paths in this table are relative to `extensions/frame/benchmarks/`.

| Report | Raw records and scope |
| --- | --- |
| [Early Arrow report](../../extensions/frame/benchmarks/arrow_benchmark_results.md) | `arrow_benchmark_results.csv`; 3 October, HG `b5a241f6`, Release extension with Debug HG. Keep separate from matching-Release results. |
| [22 October baseline](../../extensions/frame/benchmarks/results/2026-10-22/README.md) | `results/2026-10-22/{expression-release-arrow-off,arrow-release-arrow-on}.{md,csv}`; frame `ad4ad92c`, HG `21a0f87c`. Includes `profiling.md` and `profile-instructions.csv`. |
| [23 October combined optimizations](../../extensions/frame/benchmarks/results/2026-10-23/README.md) | `results/2026-10-23/comparison.csv`, `tables.md`, three before/after expression and Arrow runs, runtime manifest. Frame `47adc693` → `2f180034`, HG `5d48e914`. |
| [Bounded validity search](../../extensions/frame/benchmarks/results/2026-10-23/bounded-search/README.md) | `results/2026-10-23/bounded-search/`: comparison, tables, three before/after Arrow runs, manifest. Frame `2f180034` → `3b59d9d7`. |
| [Compaction](../../extensions/frame/benchmarks/compaction.md) | `compaction_benchmark_results.csv` at `cda84197`; `compaction_constructed_cells_{before,after}.csv` at `a52607e2`/`f43b7859`. Bitmap timing changes are not causally isolated. |
| [Range compaction](../../extensions/frame/benchmarks/results/2026-10-24/compaction-range/README.md) | `results/2026-10-24/compaction-range/{before,after}.csv` and metadata; candidate `d8dbd8c3`. Consumer-plus-maintenance totals add separately measured medians. |
| [Incremental sums](../../extensions/frame/benchmarks/incremental_sum.md) | `results/2026-10-27/incremental-sum.csv` and `results/2026-10-28/incremental-sum-nodes.csv`; complete cycles and node profiles are separate runs. |
| [Native grouped aggregation](../../extensions/frame/benchmarks/group_by.md) | `results/2026-11-04/group-by{,-nodes}.csv`; original `*-run1.csv` retained separately. Final node profiles validate same-cycle containment. |
| [Overflow recovery](../../extensions/frame/benchmarks/group_by.md#overflow-recovery) | `results/2026-11-04/group-by-overflow-{before,after}.csv`; numerical change `384de57e7`. Only the adapter result sweep is timed. |
| [Arrow grouped comparison](../../extensions/frame/benchmarks/group_by_arrow.md) | `results/2026-11-04/group-by-arrow.csv`, `group-by-comparison-bg.csv`, and `group-by-comparison-bg-nodes.csv`; comparator `77b109bcf`. Use these fresh comparison runs together. |

## Archived experiments

These records describe earlier representations and prototypes. They remain
historical evidence; the active frame benchmarks above are the reproduction path.

| Location under `extensions/frame/archive/` | Records |
| --- | --- |
| [Foundation shared-view benchmark](../../extensions/frame/archive/benchmarks/foundation-benchmark/shared-view.md) | `benchmarks/foundation-benchmark/shared-view.{md,csv}` |
| [Schema/storage baseline](../../extensions/frame/archive/benchmarks/schema-hooks-consumer/frame-schema/baseline.md) | `benchmarks/schema-hooks-consumer/frame-schema/{baseline,after-storage}.{md,csv}` |
| [Arrow probe](../../extensions/frame/archive/experiments/frame_arrow_probe/benchmark_results.md) | `experiments/frame_arrow_probe/benchmark_results.md`, `benchmark.py`, and `benchmark.sh`; prototype comparison, not the current frame API |

## Recording new results

Keep the source revision, local modifications, adapter revision, compiler flags,
runtime libraries, CPU placement, workload size, and timing boundaries with each
result. Preserve raw samples and validation outcomes, including failed attempts
that explain excluded data. Commit new results alongside their harness or report.

For map runs, use the [existing result schema](../../benchmarks/results/high_count_map/README.md).
For other suites, keep dated records beside their existing result family. Update
the published page when a new result changes the useful application guidance;
retain historical measurements and their original conditions.
