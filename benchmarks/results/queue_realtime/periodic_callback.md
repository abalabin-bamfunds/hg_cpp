# Periodic callback polling investigation — 2026-10-21

The per-cycle clock read accounts for most of the callback overhead in the
[queue and real-time baseline](README.md). The runner's enabled check and
deadline comparison add no material cost in isolation.

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

Production behavior is unchanged. Selecting a polling stride requires deciding
whether the callback may run at a later cycle boundary after its deadline.
Preserving the current check at every boundary retains the clock-read cost;
a helper timer thread would introduce separate scheduling and lifetime costs.

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
