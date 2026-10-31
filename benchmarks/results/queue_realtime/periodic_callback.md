# Periodic callback polling — 2026-10-21

Eight-cycle polling reduces the median paired callback cost from 32.395 to
5.123 ns/cycle, approximately 84%, in the simulation timer-chain benchmark.

## Graph comparison

The comparison uses GCC 14.3, `-O3 -DNDEBUG`, no LTO, and CPU affinity 0 on the
same VM as the [queue and real-time baseline](README.md). Both executables use
the same benchmark source and candidate shared libbg for common runtime code.
Each executable defines its simulation engine: the baseline uses the engine
and callback runner from `06f12da7`; the candidate uses `9473d53d`.

Each variant has 21 measured pairs: three invocations of seven pairs, after
warmup, with 1,000,000 timer firings per run. Invocations alternate baseline and
candidate order; pairs alternate disabled and enabled callback order. The
enabled callback increments a counter every 50 ms. Timing includes wiring,
start, and stop. Medians below pool all 21 samples for each setting.

| Setting | Per-cycle polling, median ns/cycle | Eight-cycle polling, median ns/cycle |
|---|---:|---:|
| Callback disabled | 113.066 | 112.330 |
| Callback enabled | 145.581 | 117.482 |
| Paired enabled minus disabled | 32.395 | 5.123 |

The median paired increase falls from 28.6% to 4.6%. The enabled median cycle
cost falls by 19.3%. Disabled sample ranges are 112.553–155.229 and
111.618–184.204 ns/cycle; enabled ranges are 144.350–192.923 and
116.697–194.617 ns/cycle, respectively. Paired increases range from
28.656–48.152 ns/cycle before and −31.446–26.278 after. The wide ranges and
negative paired samples show timing variation; the medians describe this
workload rather than a bound on overhead.

All timer counts pass. Enabled callbacks run four or five times per measured
run, including lifecycle calls. No invocation increases cgroup throttling
counters. Cgroup CPU pressure `some` increases by 0.291–1.339 ms during each
1.6–2.2-second invocation.

The [raw comparison](../periodic_callback_overhead/9473d53d89440ff11d9b6aca49f8cf62bdbb07d2-timer-ab.json)
preserves source snapshots, exact build and run commands, binary and library
hashes, all samples, and environment counters. The paired workloads can be run
with `taskset -c 0 build/benchmark/benchmarks/bg_queue_realtime_benchmark --timer-only`;
build instructions are in [queue_realtime.md](../../queue_realtime.md).

## Clock-read isolation

The per-cycle clock read accounts for most of the callback overhead in the
original baseline. The runner's enabled check and deadline comparison add no
material cost in isolation.

A standalone experiment uses GCC 14.3, `-O3 -DNDEBUG`, the same VM without
explicit CPU affinity, and seven samples of 1,000,000 loop iterations.
Each iteration updates a checksum.
Strided cases retain a countdown whose branch controls a clock read. These
measurements isolate polling cost; they are not graph-cycle measurements.

| Polling operation | Median ns/iteration |
|---|---:|
| `steady_clock::now()` every iteration | 26.019 |
| `PeriodicCallbackRunner::run_if_due()` every iteration | 26.010 |
| Clock read every 8 iterations | 3.421 |
| Clock read every 32 iterations | 1.117 |
| Clock read every 64 iterations | 0.843 |

The original graph benchmark measured a median paired increase of
34.408 ns/cycle. The isolated clock cost explains approximately 76% of that
increase. This experiment does not establish the cause of the remaining
difference between the two workloads.

## Cadence tradeoff

A fixed stride of 8 removes most clock reads with one countdown. It can delay a
due callback by up to 7 additional complete cycles. A stride of 32 allows
31 additional cycles; if each takes 10 ms, the additional delay can approach
310 ms. An adaptive stride also has this risk when cycle duration increases
after its most recent clock check. Neither method bounds lateness in wall time.

This matters because Python installs a periodic callback for every graph run
and uses it to process signals, including Ctrl-C. Startup, termination, callback
exceptions, and idle realtime waits also need their existing behavior.

The implementation in `9473d53d` checks after every eight complete outer cycles
during continuous evaluation and before and after every realtime wait. The
[decision record](../../../docs/development/decisions/0076-periodic-callback-polling.md)
states the timing tradeoff. Callback state remains local to the engine run.

## Reproduction

[Experiment source](../../periodic_callback_overhead_experiment.cpp), revision
`b176642a`, and [raw samples](../periodic_callback_overhead/gcc-14.3-linux-x86_64-2026-10-21.txt)
preserve the investigation. All sample checksums match the no-poll control.

```sh
c++ -std=c++23 -O3 -DNDEBUG -pthread -Iinclude benchmarks/periodic_callback_overhead_experiment.cpp -o build/benchmark/benchmarks/bg_periodic_callback_overhead_experiment
build/benchmark/benchmarks/bg_periodic_callback_overhead_experiment
```

`--quick` reduces the sample count and iterations. The Linux coarse-clock case
is a diagnostic control and is not a portable replacement for the production
steady-clock check.
