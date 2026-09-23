# High-count map benchmark

## Purpose

This benchmark measures sparse scheduling, map lifecycle, maps feeding maps,
and a map inside a map. It uses public graph construction and runtime APIs so
the same workload can be implemented by other graph engines.

The comparison profile contains 250,000 active inner graphs and evaluates
10,000 outer cycles. Smaller profiles preserve the topology for development.

## Topology

All values and keys are non-negative signed 64-bit integers.

The flat branch contains 80,000 source items. Its first map applies stage A:

```text
a1 = value + bias
a2 = a1 * 3
a3 = a2 + key
a4 = a3 * 5
output = a4 + 7
```

Its second map consumes stage A and applies stage B:

```text
b1 = value + bias
b2 = b1 * 7
b3 = b2 + key
b4 = b3 * 3
output = b4 + 11
```

The nested branch contains 10,000 parent items with eight child items each.
A parent map computes `seed = (bias + parent_key) * 5`. A child map inside
each parent graph computes:

```text
c1 = value + seed
c2 = c1 * 3
c3 = c2 + child_key
c4 = c3 + bias
output = c4 * 7
```

The active inner graph count is therefore:

```text
80,000 stage A + 80,000 stage B + 10,000 parents + 80,000 children = 250,000
```

Each arithmetic operation is a separate graph node. The output sinks observe
stage B and child-map changes.

## Workload

The engine-independent workload generator in
[`high_count_map_benchmark.cpp`](high_count_map_benchmark.cpp) defines the
initial values, update values, selected keys, lifecycle operations, and
expected checksum. Port its workload and checksum code without changing the
constants or operation order.

The `250k` profile applies 250 flat updates and 250 child updates per cycle.
Every 100-cycle period performs these lifecycle operations:

- phase 20 removes 250 flat items and 25 complete parents;
- phase 21 restores those items;
- phase 40 removes one child from each of 100 parents;
- phase 41 restores those children.

Cycle 50 of every 1,000-cycle period increments the shared bias. This schedules
every active inner graph.

The population cycle is not one of the measured cycles. The comparison run
uses `--profile 250k --cycles 10000`.

## Validation

Record every stage-B and child-output change during measured cycles, including
removals. Hash this tuple in order:

```text
(cycle, branch, parent_key, item_key, present, value)
```

Branch 1 is the flat output and branch 2 is the child output. The benchmark
source defines the unsigned 64-bit mixing and accumulation operations. A valid
comparison run produces:

```text
count = 6705000
sum   = 18255748205643653813
xor   = 10077992542007829525
```

Reject a run whose checksum differs.

## Measurements

Use a release build. Pin repeated comparison runs to one otherwise idle CPU
when the platform supports affinity. Report nanoseconds for:

- workload generation;
- wiring or graph construction;
- runtime construction and start;
- initial population;
- all measured cycles;
- final stop.

Measure each outer cycle from input submission through completion of both
output sinks. Report count, total, mean, median, 95th percentile, 99th
percentile, and maximum for these classes:

- ordinary;
- flat and parent removal;
- flat and parent restoration cleanup;
- child removal;
- child restoration cleanup;
- bias broadcast.

Record process resident set size (RSS) before wiring, after wiring, after an
empty runtime starts, after population, at the process peak, and after stop.
`populated_rss_delta_bytes` is populated RSS minus empty-runtime RSS.
`bytes_per_active_inner_graph` divides this delta by 250,000. This measurement
includes engine-inserted nodes and all runtime support data.

Do not inspect engine nodes, allocators, or runtime storage to obtain the
required measurements. An engine that cannot separate a timing phase records
`null` and states which surrounding phase includes it.

## Porting procedure

1. Implement the topology with the engine's public graph and map interfaces.
2. Port the engine-independent workload generator and checksum from the C++
   benchmark.
3. Run the smoke profile and its manifest validation before a comparison run.
4. Run the `250k` profile for 10,000 measured cycles in a release build.
5. Confirm the output checksum before accepting timing or memory results.
6. Add the raw JSON result under
   [`results/high_count_map`](results/high_count_map/README.md), keyed by the
   engine source commit used to build the runtime.

Do not simplify the graph, combine arithmetic nodes, exclude engine-created
runtime objects from memory, or replace incremental evaluation with a batch
calculation.

## Running BG

Build and validate a small run:

```bash
cmake --preset benchmark
cmake --build --preset benchmark --target bg_high_count_map_benchmark
build/benchmark/benchmarks/bg_high_count_map_benchmark --verify-workload
build/benchmark/benchmarks/bg_high_count_map_benchmark --profile smoke --cycles 120
```

Run the comparison profile:

```bash
taskset -c 0 build/benchmark/benchmarks/bg_high_count_map_benchmark --profile 250k --cycles 10000
```

Use an available CPU instead of CPU 0 when required. Redirect standard output
to a temporary file only after the process exits successfully. Add the source
commit and environment metadata required by the results format before moving
the file under `results/high_count_map`.
