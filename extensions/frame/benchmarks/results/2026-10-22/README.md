# Frame benchmark results: 2026-10-22

These results measure the frame implementation at `ad4ad92c`, against BG main
baseline `21a0f87c`. The benchmark and extension used GNU C++ 14.3.0 with
`-O3 -DNDEBUG`; the installed BG library used the matching Release build. The
Arrow run used Arrow and ArrowCompute 24.0.0 from the `bg-build` Conda
environment. The host was an x86_64 Linux KVM VM with an AMD EPYC 7R13
processor. Each benchmark ran as one process. No concurrent builds by our agents
ran during measurement, and no CPU affinity was set.

The [expression benchmark](expression-release-arrow-off.md) ran without Arrow.
The [Arrow benchmark](arrow-release-arrow-on.md) used the Arrow-enabled Release
consumer. Each report contains the workload, row counts, update counts, and
measured timings. Steady-cycle timings are medians from one process run with two
warmup cycles and eleven measured cycles. Lifecycle timings are single samples.

The expression benchmark's median steady-cycle times were:

| Operation | Median range (ms) |
| --- | ---: |
| Payload update | 0.144–0.145 |
| Predicate update preserving membership | 0.266–0.280 |
| Predicate update changing membership | 1.792–1.845 |
| Threshold updates | 9.498–11.693 |
| 1,000 additions and 1,000 removals | 2.566–2.735 |

Source mutation timings include keyed add, set, and erase work, including key
index maintenance. The add/remove case keeps the row count stable while its
active and removed key layout evolves over the measured cycles. The count sink
enumerates changed and removed filter IDs inside the timed graph evaluation;
count and delta validation runs after each timed evaluation.

In the Arrow report, scattered selection of 49,168 rows visited 49,168 chunks.
Borrowed Arrow plus sum had consumer medians of 18.030–18.584 ms; dense gather
plus sum had medians of 1.229–1.245 ms. With dense selection of 100,000 rows
across 15 chunks, borrowed Arrow plus sum measured 0.976 ms, compared with
0.020 ms for summing a prebuilt plain Arrow array and 0.056 ms for building an
Arrow array from the existing vector, summing, and destroying the array. The
dense consumer path still has substantial cost with only 15 chunks. These
timings do not isolate traversal or export work. Do not subtract medians to
infer phase timings. Subsequent [Callgrind profiles](profiling.md) identify
instruction costs in traversal, validity construction, and Arrow chunk handling.

The filter implementation rebuilds its selected-order sequence by scanning the
source row set when membership changes, preserving source order. The profiles
confirm that this scan dominates small membership-changing updates and also
runs unnecessarily on full predicate reevaluation with unchanged membership.

## Build and run commands

Run these commands from the `.lanes/frame-field-access` worktree root. The build
trees were configured for Release with the matching BG install at
`dependencies/bg-frame-core-release-install`; the Arrow-enabled tree also uses
Arrow 24.0.0 from the `bg-build` Conda environment. The compiler and Arrow
packages come from `bg-build`; CMake and build commands run through `hg-build`.

```sh
BG_FRAME_RELEASE_PREFIX="$PWD/dependencies/bg-frame-core-release-install"
BG_FRAME_BUILD_PREFIX=/home/abalabin/miniforge3/envs/bg-build
```

```sh
conda run --no-capture-output -n hg-build cmake -S extensions/frame \
  -B build/tsf-row-release-arrow-off -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$BG_FRAME_RELEASE_PREFIX;$BG_FRAME_BUILD_PREFIX" \
  -DBG_FRAME_BUILD_ARROW=OFF -DBG_FRAME_BUILD_BENCHMARKS=ON -DBUILD_TESTING=ON
conda run --no-capture-output -n hg-build cmake --build build/tsf-row-release-arrow-off \
  --target bg_frame_expression_benchmark -j2
build/tsf-row-release-arrow-off/bg_frame_expression_benchmark \
  --report extensions/frame/benchmarks/results/2026-10-22/expression-release-arrow-off.md \
  --csv extensions/frame/benchmarks/results/2026-10-22/expression-release-arrow-off.csv
```

```sh
conda run --no-capture-output -n hg-build cmake -S extensions/frame \
  -B build/tsf-row-release-arrow-on -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$BG_FRAME_RELEASE_PREFIX;$BG_FRAME_BUILD_PREFIX" \
  -DArrow_DIR="$BG_FRAME_BUILD_PREFIX/lib/cmake/Arrow" \
  -DBG_FRAME_BUILD_ARROW=ON -DBG_FRAME_BUILD_BENCHMARKS=ON -DBUILD_TESTING=ON
conda run --no-capture-output -n hg-build cmake --build build/tsf-row-release-arrow-on \
  --target bg_frame_arrow_benchmark -j2
build/tsf-row-release-arrow-on/bg_frame_arrow_benchmark \
  --report extensions/frame/benchmarks/results/2026-10-22/arrow-release-arrow-on.md \
  --csv extensions/frame/benchmarks/results/2026-10-22/arrow-release-arrow-on.csv
```
