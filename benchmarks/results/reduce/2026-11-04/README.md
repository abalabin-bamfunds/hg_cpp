# Native dictionary reduction

The [published benchmark page](../../../../docs/user/benchmarks.md#reduction)
summarizes the application costs. This directory retains the measurement
conditions and harness output.

- [Environment](environment.json): source revisions, compiler and build flags,
  dependency revisions, loaded libraries and hashes, CPU placement, and timestamps.
- [Run summaries](run-summaries.json): commands, return codes, stderr, host-load
  snapshots, timing distributions, memory observations, and validation results.
  The harness reports distributions; individual cycle timings are not retained.

## Workload and method

Engine and harness sources match `a2b97f570`. The measuring checkout was
`aec316df1`, with documentation changes only. The build uses GCC 14.3.0,
C++23, `-O3 -DNDEBUG`, shared HG, and no LTO on an AMD EPYC 7R13 Linux KVM VM.
All benchmark processes run sequentially with affinity to CPU 31. Other host
workloads are uncontrolled; affinity does not provide an exclusive CPU.

The reducer graph contains one integer-addition node and combines a native
`TSD<i64, TS<i64>>`. This uses compact scalar reduction. Each size has three
fresh processes in reduction mode and three source-only controls. Every process
measures 100 single-key updates, 100 consecutive 1% bursts, and 100 remove/restore
pairs affecting 1% of the dictionary each time. Each reduction process checks
401 aggregate outputs, including initial population, against independently
generated sums. Source-only mode checks completion, without aggregate validation.

Two separate 10,000-item smoke processes use two update samples and one lifecycle
pair; their records have `smoke: true` and are excluded from results. Measured
processes discard no warmup cycles. For each size, the process order is
reduce/source, source/reduce, reduce/source.

Update tables use the median of three process medians, with each process median
computed from 100 cycles of that update kind. Initialization and memory figures
use medians across the three reduction processes. The summaries do not pool
latency samples or subtract source-only timings from reduction timings.

The timed interval is `RuntimeGraph::evaluate()`, including source mutation,
reduction and the validating sink when present, output publication, and cleanup.
Workload generation, wiring/compilation, construction, start, initial population,
and stop are outside update timings. Their lifecycle timings remain in the
records. The source-only graph has no reducer or sink.

The populated RSS increase is populated resident memory minus empty-runtime
resident memory. It includes the source dictionary, reducer storage, and allocator
effects. It is not reducer-only memory or peak process RSS.

## Reproduction

Build and smoke-check the existing harness from a source checkout:

```sh
cmake --preset benchmark
cmake --build --preset benchmark --target hg_reduce_benchmark
taskset -c 31 build/benchmark/benchmarks/hg_reduce_benchmark --size 10000 --samples 2 --lifecycle-rounds 1
taskset -c 31 build/benchmark/benchmarks/hg_reduce_benchmark --size 10000 --mode source --samples 2 --lifecycle-rounds 1
```

For each of 10,000, 100,000, and 1,000,000 items, run both modes three times in the
order above. Replace the size and mode in this command:

```sh
taskset -c 31 build/benchmark/benchmarks/hg_reduce_benchmark \
  --size 100000 --mode reduce --samples 100 --lifecycle-rounds 100
```

Select an available CPU on another host. The environment record contains the
dependency paths used for this build; CMake fetched no new dependency sources.
