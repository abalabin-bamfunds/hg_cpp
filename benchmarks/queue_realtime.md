# Queue and real-time benchmark

`bg_queue_realtime_benchmark` measures queue write throughput, graph drain cost,
real-time queue delivery latency, and simulation timer-cycle overhead with and
without a 50 ms periodic callback. Build it with the `benchmark` preset:

```sh
cmake --preset benchmark
cmake --build --preset benchmark --target bg_queue_realtime_benchmark
build/benchmark/benchmarks/bg_queue_realtime_benchmark
```

The default run uses 100,000 messages, 1,000 latency samples per run, 1,000,000
timer cycles, and seven measured samples. `--quick` uses smaller workloads for
smoke validation. `--timer-only` runs the paired simulation timer samples without
the queue workloads. `--messages`, `--latency-messages`, `--timer-cycles`, and
`--samples` override the corresponding defaults. Output is JSON Lines with raw
samples and summaries.

Queue writes use prebuilt scalar literals and one or four producers. The
literal batches own stable scalar backing vectors because scalar literals
reference their source values. The benchmark drains every write after timing
and checks the total count, checksum, and each producer's sequence order. Graph
drain preloads standard, eliding, and
batch queues before timing evaluation. It reports output ticks as well as
nanoseconds per input message; standard emits one tick per message while
eliding and batch emit one tick for the preloaded scalar series. Count,
checksum, and final values validate each drain.

The real-time latency workload starts its measurement before constructing the
literal and calling `Queue::write`, waits for downstream observation, then
sleeps for a 1 ms idle gap outside the measured interval. This measures
write-to-delivery latency, including literal construction, queue wakeup, and
the downstream observer lock; it does not measure a guaranteed sleep duration.
Each sample reports p50, p95, p99, maximum, and chronological raw latencies.
The simulation timer workload uses
the same exact-timer chain for both callback settings and alternates their run
order between samples. The callback interval is 50 ms.
