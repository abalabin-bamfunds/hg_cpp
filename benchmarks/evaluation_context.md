# Evaluation context benchmark

`bg_evaluation_context_benchmark` measures child-context construction and graph
evaluation that reads evaluation time. Build it with the `benchmark` preset:

```sh
cmake --preset benchmark
cmake --build --preset benchmark --target bg_evaluation_context_benchmark
build/benchmark/benchmarks/bg_evaluation_context_benchmark
```

Each workload reports nine samples and their median, minimum, and maximum.
Construction creates one million child contexts. Flat evaluation runs 200,000
cycles through a node that reads evaluation time and schedules its next cycle.
Nested evaluation runs the same node through an owner-bound inner graph.
Checksums verify construction counts, evaluation counts, and evaluation times.
Timings include engine startup and shutdown.

## Context split measurement, 2026-10-19

Base revision: `60a73309`. Platform: AMD EPYC 7R13, GCC 14.3.0,
Release (`-O3 -DNDEBUG`), pinned to CPU 0.

The implemented layout keeps evaluation time in `EvaluationContext`. The
comparison layout moves the field into `GlobalEvaluationContext`, removes it
from the local context and child constructors, and sets it on outer entry.

| Layout | `EvaluationContext` | `GlobalEvaluationContext` |
| --- | ---: | ---: |
| Before the split | 88 bytes | — |
| Local evaluation time | 72 bytes | 48 bytes |
| Shared evaluation time | 64 bytes | 56 bytes |

`RuntimeGraph` remains 96 bytes and `RuntimeGraphEvaluator` remains 40 bytes.
The global context is stack storage shared by nested invocations.

Each row below is the median of nine samples. The first pair ran shared then
local; the second pair reversed that order. Both used preserved executables
and their matching shared libraries.

| Pair | Evaluation time | Child construction, ns | Flat cycle, ns | Nested cycle, ns |
| --- | --- | ---: | ---: | ---: |
| 1 | Shared | 7.87 | 124.54 | 280.29 |
| 1 | Local | 7.94 | 125.97 | 281.44 |
| 2 | Local | 7.84 | 126.96 | 281.16 |
| 2 | Shared | 10.25 | 119.58 | 265.25 |

The second shared-time construction run ranged from 7.37 to 17.43 ms per
million contexts. The other construction runs ranged from 7.80 to 9.15 ms.
This variability prevents a reliable conclusion about construction cost.
Graph evaluation did not show a consistent slowdown from shared time in these
workloads. Local evaluation time remains selected to preserve direct reads
while reducing the context by 16 bytes. These measurements do not establish an
overall evaluation speed improvement over the original context.

For saved variants, explicitly preload the matching library. The executable's
RPATH can otherwise select a rebuilt library with a different context layout.
From the build's source directory, each variant was run as:

```sh
taskset -c 0 env \
  LD_PRELOAD="$PWD/build/benchmark/context-cached/libbg.so.0.1.0" \
  LD_LIBRARY_PATH="$PWD/build/benchmark/context-cached" \
  "$PWD/build/benchmark/context-cached/bg_evaluation_context_benchmark"
```

The shared-time command uses `context-global-only` in place of `context-cached`.
The saved binaries are local build artifacts.
