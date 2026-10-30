# Bounded native validity search

This follow-up measures the bounded bitmap search added after the
[initial optimization comparison](../README.md). It compares frame revisions
`2f180034` and `3b59d9d7`. The BG baseline is `5d48e914`; the new build also
includes the utility-only change `73622fb7` (`de53c9e7` on its separate lane).

The earlier build checks native validity per cell. The new build uses
`index::first_unset(start, end)` to search bitmap words within the batch.
Both omit Arrow validity allocation when every cell is valid. Row spans,
selection storage, row order, and export ownership are unchanged.

## Results

The table reports consumer medians in milliseconds. Borrowed Arrow timings
include export, sum, and exported-object destruction.

| Workload | Consumer | Before | After | Speedup |
| --- | --- | ---: | ---: | ---: |
| 100,000 dense rows | Borrowed Arrow + sum | 0.220 | 0.032 | 6.84× |
| Clustered selection and deletion | Borrowed Arrow + sum | 0.180 | 0.081 | 2.22× |
| Scattered selection, clustered deletion | Borrowed Arrow + sum | 16.530 | 16.444 | 1.01× |
| 100,000 dense rows | Native batch sum | 0.085 | 0.086 | 0.99× |

Dense borrowed Arrow process medians ranged from 0.219–0.380 ms before and
0.025–0.034 ms after. The other consumers show no comparable improvement.
Small changes overlap process variation. Scattered selection retains 49,168
Arrow chunks; bounded validity search does not remove their allocation cost.
All runs validated their results, row counts, and chunk counts.

See [all 18 consumer comparisons](tables.md), [complete comparison data](comparison.csv),
and the [runtime manifest](runtime-manifest.txt). The CSV includes source mutation,
whole-graph, and consumer process-median ranges. Use each paired comparison
directly; speedups from separate comparisons do not establish a combined ratio.

## Method and verification

Each version ran in three processes, with before/after, after/before, and
before/after order. Each process excluded two warmup cycles and measured eleven
cycles. Results use the median of the three process medians. The workload,
compiler, host, and timing boundaries are described in the
[initial report](../README.md#measurement). These workloads contain no nulls;
functional tests cover mixed validity and native index boundaries.

No task builds ran during measurement. The ELF loader disabled executable
RPATH as described in the [reproduction instructions](../README.md#reproduction).
The manifest verifies the frame and BG libraries and records their hashes.

The earlier runtime is preserved under
`build/frame-optimisation-2026-10-22/pre-bounded-runtime/arrow-on`, using
`dependencies/bg-frame-core-release-install`. The new runtime is
`build/tsf-row-release-arrow-on`, using
`dependencies/bg-frame-core-bounded-release-install`.

Final verification passed Release Arrow-off 5/5, Release Arrow-on 6/6, and
Debug ASan/UBSan Arrow-on 6/6. The utility's focused Catch2 test passed 787
assertions across ten cases.
