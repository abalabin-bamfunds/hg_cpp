# Queue and real-time baseline — 2026-10-21

Native scalar queue measurements on AMD EPYC 7R13 (KVM), Linux x86-64,
GCC 14.3.0, Release `-O3 -DNDEBUG`, shared libbg, no LTO. The VM exposes
32 logical CPUs; its cgroup quota is 16 CPUs. Queue and wakeup processes use
CPUs 0–3; scheduling uses CPU 0. Threads are not individually pinned.

Engine revision: `083020613c6f1229753ab8501f2d4134de48bddf`.
Queue benchmark revision: `10481b3a2404aa11cacdd7e286df6f68b1c8f43c`.
Wakeup control and scheduling correction: `1ca7da5c5ad1b6a5e28aa84be4e35976952bba7d`.

## Queue throughput

Each case has 21 measured samples: three invocations of seven samples, after
warmup, with 100,000 `i64` input messages per sample. The table uses the median
of all 21 samples. Ranges are the minimum and maximum sample costs.

| Workload | Median ns/input message | Range ns/input message | Million input messages/s |
|---|---:|---:|---:|
| Write, one producer | 346.3 | 332.6–384.0 | 2.89 |
| Write, four producers | 303.4 | 281.8–376.8 | 3.30 |
| Drain, standard | 623.4 | 616.3–834.1 | 1.60 |
| Drain, elide | 237.9 | 236.2–281.8 | 4.20 |
| Drain, batch | 388.9 | 384.8–409.1 | 2.57 |

Writing uses prebuilt literals, includes reservation, encoding, and buffer
allocation, and has no concurrent reader or attached engine wakeup. Validation
drains the queue after timing and checks count, checksum, and per-producer order.
Four producers improve aggregate throughput by 14%; this case needs profiling
before attributing the limited scaling to a particular queue operation.

Drain timing starts after preloading and calls `RuntimeGraph::evaluate`
directly. Standard emits 100,000 scalar ticks across 100,000 cycles. Elide emits
one scalar tick. Batch emits one list tick containing all messages; its timing
includes scanning the list in the downstream node. These are separate write
and drain measurements, not a measured end-to-end throughput result.

## Real-time delivery and wakeup controls

Delivery has 21,000 measured messages, with one message in flight, 50 warmup
messages per sample, and a 1 ms idle gap outside the timed interval. Timing
starts before literal construction and `Queue::write` and ends in the downstream
observer. It includes engine wakeup and the observer mutex. The periodic
callback is disabled.

The controls use the same one-message handshake and 1 ms idle gap before each
notification, with 3,000 measured notifications per control. Percentiles below
are pooled from chronological raw samples, using index `floor(p * (n - 1))`.

| Workload | p50 µs | p95 µs | p99 µs | Maximum µs |
|---|---:|---:|---:|---:|
| Queue write to downstream observation | 18.151 | 33.262 | 42.462 | 1,216.653 |
| Plain `std::binary_semaphore` | 15.461 | 34.242 | 38.782 | 392.010 |
| `EngineWakeup` | 13.890 | 21.761 | 32.532 | 71.544 |

Wakeup and thread scheduling account for a substantial part of delivery time.
Controls run sequentially and thread placement varies, so these results do not
establish that `EngineWakeup` is faster than the plain semaphore. Maximum
latencies are observations, not bounds.

## Periodic callback and scheduling

Simulation runs an exact-timer chain for 1,000,000 cycles per sample, with
21 pairs alternating the order of disabled and enabled callback runs. Timing
includes wiring, start, and stop. The enabled callback increments a counter
every 50 ms; it runs four or five times per measured run, including lifecycle
calls.

| Setting | Median ns/cycle | Range ns/cycle |
|---|---:|---:|
| Callback disabled | 111.949 | 109.822–130.155 |
| Callback enabled | 146.207 | 141.909–156.679 |

The median paired increase is **34.408 ns/cycle (30.8%)**; paired increases
range from 12.302 to 44.730 ns/cycle. The per-cycle wall-clock check in
`PeriodicCallbackRunner::run_if_due` is a candidate for profiling.
The [polling investigation](periodic_callback.md) isolates its clock-read cost
and measures the cost of checking less often.

The scheduling benchmark has three runs of 200,000 cycles each. Dense scheduling
of 32 nodes measures 963.6 ns/cycle (range 904.2–1,249.7). Scheduling one of 32
nodes measures 77.2 ns/cycle (75.3–91.6). All evaluation counts pass.
`RuntimeGraph` is 96 bytes; measured runtime heap storage is 40 bytes through
64 nodes and 64 bytes at 65 nodes. `EvaluationContext` is 72 bytes,
`GlobalEvaluationContext` 48 bytes, and `RuntimeGraphEvaluator` 40 bytes.

## Validation and environment variability

All final queue, control, and scheduling invocations pass their value, count,
order, or checksum checks. No engine code changed. Initial harness validation
found literals borrowing expired scalar values; the benchmark now retains
stable backing storage. The initial scheduling benchmark lacked
`GraphWiring::finalize()`; that setup error is fixed and all three reruns pass.

The [initial sweep](10481b3a2404aa11cacdd7e286df6f68b1c8f43c.json) retains the
failed scheduling record and successful queue/context runs. CPU pressure during
that sweep was approximately 18–19%; queue delivery p99 was 5.28 ms and maximum
39.96 ms. Those observations are associated with contention but do not identify
the cause of each stall.

The [final rerun](1ca7da5c5ad1b6a5e28aa84be4e35976952bba7d-rerun.json) supplies
the tables above and records per-invocation CPU counters and executable hashes.
No invocation increased cgroup throttling counters. Cgroup CPU pressure `some`
increased by 23–30 ms during each approximately 13-second queue invocation.
This is a native scalar baseline; Python, dictionary deltas, and simultaneous
producer/consumer throughput remain unmeasured. There is no prior-version
comparison from which to infer a regression.

## Reproduction

Use a GCC 14.3 environment for this baseline. Build instructions for the queue
workload are in [queue_realtime.md](../../queue_realtime.md).

```sh
cmake --preset benchmark
cmake --build --preset benchmark --target bg_queue_realtime_benchmark bg_runtime_scheduling_benchmark bg_evaluation_context_benchmark
c++ -std=c++23 -O3 -DNDEBUG -pthread -Iinclude benchmarks/wakeup_latency_control.cpp -o build/benchmark/benchmarks/bg_wakeup_latency_control
taskset -c 0-3 build/benchmark/benchmarks/bg_queue_realtime_benchmark
taskset -c 0-3 build/benchmark/benchmarks/bg_wakeup_latency_control
taskset -c 0 build/benchmark/benchmarks/bg_runtime_scheduling_benchmark
```

Repeat each measured invocation three times. `--quick` validates the new
workloads with smaller counts. Raw artifacts retain exact commands and outputs;
the initial artifact also records compiler and CPU details.
