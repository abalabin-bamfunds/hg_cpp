# Frame optimization results: 2026-10-23

The combined changes reduce full predicate reevaluation, reuse filter selection
storage, expose consecutive physical slots through ordered row spans, and avoid
temporary Arrow validity allocation for all-valid batches. Public node APIs and
the separate borrowed and gather-copy Arrow contracts are unchanged.

See [all workload comparisons](tables.md), [comparison data](comparison.csv),
and the [runtime library manifest](runtime-manifest.txt).
The later [bounded native validity search comparison](bounded-search/README.md)
measures its additional Arrow export improvement separately.

## Source and build

The baseline frame revision is `47adc693`; the measured optimized revision is
`2f180034`. The optimization commits are `e858e5fa` (filter reevaluation),
`38e0248b` (row spans), and `2f180034` (Arrow validity allocation).
Both versions use the fixed BG revision `5d48e914`, installed in
`dependencies/bg-frame-core-release-install`. The baseline executables and their
matching extension libraries were preserved before rebuilding. The manifest
records their hashes and resolved libraries.
The measured optimized binaries are also preserved under
`build/frame-optimisation-2026-10-22/pre-bounded-runtime`. These measurements
predate the bounded bitmap search added after this comparison.

The compiler is GNU C++ 14.3.0, with `-O3 -DNDEBUG`. The Arrow benchmark uses
Arrow and ArrowCompute 24.0.0. The host is an x86_64 Linux KVM VM with an AMD
EPYC 7R13 processor. No CPU affinity was set, and this task ran no builds during
measurement.

## Measurement

Each benchmark ran three times per version. Each round ran expression and Arrow
pairs; version order was before/after, after/before, then before/after.
Each process excluded two warmup cycles, then measured eleven steady cycles.
The expression benchmark also times a separate first evaluation before those
warmup cycles and excludes it from steady timings.
The comparison uses the median of the three process
medians. `comparison.csv` also records the range of process medians. The raw
`before-*-{1,2,3}` and `after-*-{1,2,3}` Markdown and CSV files retain complete
per-process results, including lifecycle samples and controls.

Expression timings measure complete graph evaluations: source mutation,
predicate evaluation, selection maintenance, and the delta-count sink. The
workload contains 90,000 live rows after initial deletions.

Arrow consumer timings measure traversal and summation. Borrowed and gather-copy
paths include export allocation, Arrow sum, and exported-object destruction.
Native sum traverses native batches. Graph timings additionally include source
mutation and filter maintenance. These sums rescan all selected cells each cycle.
Graph construction and initial source seeding are excluded. Plain Arrow controls
operate outside the graph on a prebuilt array or a pre-existing dense vector.
Do not subtract medians to infer individual phase costs.

The results measure the combined implementation. They do not isolate the gain
from each optimization. Small differences can reflect execution variance; one
optimized clustered payload run had a process median of 0.153 ms, compared with
about 0.075 ms in its other two runs.

## Implementation and limits

Full predicate reevaluation receives the source row set directly. It rebuilds
selected order only when membership or the source binding requires it. Partial
predicate updates keep the existing incremental request path.

A filter reuses an initialized prefix of selected-ID storage. Each ID storage
block carries a consecutive-slot mapping when its source spans establish one.
Matching columns traverse slots directly; other mappings retain ID lookup and
existing coalescing or gathering. No separate scan establishes these mappings.
The [decision record](../../../docs/decisions/selected-row-span-metadata.md)
defines ownership and metadata scope. Capacity remains retained at the selection
high-water mark; these benchmarks do not measure peak memory.

Borrowed Arrow export allocates validity only after finding a null. Validity
search remains bounded to each batch and checks native validity per cell.
The all-valid workloads here do not quantify mixed-null export performance;
tests cover mixed validity, all-null buffers, and Boolean bit offsets.

Dense native traversal and borrowed Arrow consumption improve substantially.
Scattered selection still produces one Arrow array per selected run: the 50%
scattered cases retain 49,168 final-cycle chunks. Their borrowed consumer times
remain around 16 ms, and the small measured changes overlap process variation.
Gather-copy still produces one owning chunk and is much faster in these cases.
No gather is hidden inside borrowed export.

The clustered-selection/clustered-deletion case changes from 14 to 15 final
native batches or borrowed chunks because selected-ID block boundaries now also
split proven spans. Row order, row counts, values, and nulls are preserved.

## Reproduction

Run from `.lanes/frame-field-access`. Build the current consumers with the
matching Release BG install and `-j12`, using the configurations documented in
the [earlier benchmark notes](../2026-10-22/README.md#build-and-run-commands).

The executables contain absolute ELF `DT_RPATH` entries. To run preserved
binaries against preserved libraries, launch both versions through the loader
with the executable's RPATH disabled. `LD_LIBRARY_PATH` alone does not override
this RPATH. For example:

```sh
BG_FRAME_RUNTIME="$PWD/build/frame-optimisation-2026-10-22/before-runtime/arrow-on"
BG_FRAME_LIBRARY_PATH="$BG_FRAME_RUNTIME:$PWD/dependencies/bg-frame-core-release-install/lib64:/home/abalabin/miniforge3/envs/bg-build/lib:/home/abalabin/miniforge3/envs/hg-build/lib"
/lib64/ld-linux-x86-64.so.2 --inhibit-rpath '' \
  --library-path "$BG_FRAME_LIBRARY_PATH" --list \
  "$BG_FRAME_RUNTIME/bg_frame_arrow_benchmark"
/lib64/ld-linux-x86-64.so.2 --inhibit-rpath '' \
  --library-path "$BG_FRAME_LIBRARY_PATH" \
  "$BG_FRAME_RUNTIME/bg_frame_arrow_benchmark" \
  --report /tmp/before-arrow.md --csv /tmp/before-arrow.csv
```

For the measured optimized run, set `BG_FRAME_RUNTIME` to
`$PWD/build/frame-optimisation-2026-10-22/pre-bounded-runtime/arrow-on` and
rebuild `BG_FRAME_LIBRARY_PATH`.
For expressions, use the corresponding `arrow-off` directory and
`bg_frame_expression_benchmark`. Verify that `--list` resolves each frame
library to the selected runtime directory before measurement.
