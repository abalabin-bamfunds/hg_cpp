# Frame profiling: 2026-10-22

The main costs are row traversal, selection maintenance, and Arrow chunk
construction. The current block kernels account for a smaller share of the
profiled work. The proposed changes below have not been implemented.

## Measurement

Callgrind 3.25.1 profiled the existing Release binaries at `67c27ed6`, whose
frame runtime code is at `ad4ad92c`. Builds, dependencies, workloads, and native
elapsed times are described in the [benchmark notes](README.md).

Collection covered each outer `RuntimeGraph::evaluate(time, inputs)` call,
including source mutation, node evaluation, publication, and consumers. Wiring,
compilation, start, stop, and validation after evaluation were outside collection.
Results below use eleven steady cycles per case, excluding initialization and
warmup. Both benchmark processes completed successfully.

Percentages measure executed instructions across those eleven cycles. They do
not measure elapsed-time shares. Self cost excludes callees; inclusive cost
includes callees. Inclusive entries can overlap. The accompanying
[CSV](profile-instructions.csv) contains all 30 cases, dump numbers, median/min/max
cycle instruction counts, and selected cost shares. Its `source` column measures
the frame source, excluding the separate clock and threshold nodes. Its
`kernels_self_pct` column measures the built-in arithmetic and comparison kernels.
Parsing sums self costs to the Callgrind summary for every selected dump and
combines repeated function names before computing percentages.

Callgrind changes execution and allocation behavior. Valgrind reported a `brk`
growth limitation and continued with its fallback. Use the earlier native runs
for elapsed times; do not derive speedups from these instruction counts.

## Findings

### Dense consumers still reconstruct batches one row at a time

The dense case contains 100,000 live rows and produces 15 native batches or
borrowed Arrow chunks. Membership remains unchanged during steady cycles.

| Consumer | Median graph instructions | Consumer inclusive share |
| --- | ---: | ---: |
| Native sum | 11,533,101 | 98.05% |
| Borrowed Arrow plus sum | 13,621,685 | 98.34% |
| Gather Arrow plus sum | 15,680,774 | 98.57% |

`NativeColumnData::for_each_batch(rows, visible_rows, callback)` enumerates the
filter's selected IDs and calls `slot_of(id)` once per row to reconstruct
contiguous batches. The filter supplies a generic `RowSet`, so the existing
native run traversal does not apply even when all rows are selected.

In the borrowed Arrow case, this produces 100,000 slot lookups for 15 chunks.
The filter row iterator, batch-formation callback, and slot lookup account for
64.0% of whole-cycle instructions in their combined self costs. `make_array`
accounts for another 33.2% inclusively: it allocates and fills a temporary
validity bitmap by checking every cell, then discards it because all cells are
valid. Arrow's complete `CallFunction("sum", ...)` accounts for 0.98% inclusively.

See [native batch traversal](../../../include/bg/frame/column.hpp) and
[Arrow export](../../../include/bg/frame/arrow.hpp).

### Scattered selection makes Arrow object and chunk overhead dominant

With 50% scattered selection and clustered deletion, borrowed export creates
one Arrow array per selected row. It averages 49,488 arrays per steady cycle;
the final cycle has the 49,168 chunks recorded in the native timing report.

`make_array` accounts for 40.5% inclusively. Arrow's scalar aggregation executor
accounts for 23.3% inclusively and initializes aggregation state for each chunk.
Allocator work occurs in both paths and during destruction: the self costs of
`malloc`, `_int_malloc`, `free`, `_int_free`, and `malloc_consolidate` together
account for 43.8% of whole-cycle instructions. These shares overlap the two
inclusive entries and must not be added to them.

Gather export produces one Arrow chunk. Its median graph instruction count is
28.4 million, compared with 202.6 million for borrowed export. The earlier native
consumer medians were 1.245 ms and 18.584 ms respectively. Gathering still pays
for row traversal, but avoids constructing and executing over thousands of Arrow
objects. These costs depend on fragmentation, not only the selected row count.

### Small membership changes trigger a complete selected-order rebuild

The predicate-changing case modifies 1,000 scores: 500 rows enter and 500 leave
the selection. It reevaluates those rows incrementally, then scans all 90,000
source rows and rebuilds the 45,000 selected IDs to preserve source order.

For clustered holes, source enumeration and its order-building callback account
for 70.5% inclusively. Clearing the previous sequence calls the bunch removal
operation 45,000 times and accounts for another 8.0% in self cost. The two block
kernels account for 0.97% in self cost. This explains why membership-changing
updates cost much more than predicate updates that preserve membership.

See `rebuild_selected_order` and `clear_selected_order` in
[view storage](../../../include/bg/frame/frame.hpp).

### Threshold changes miss the native traversal path

The threshold-preserving case reevaluates all 90,000 rows. The frame source does
not run in these steady cycles. The filter:

1. Inserts all source IDs into `predicate_rows`.
2. Wraps that index as a generic `RowSet`.
3. Traverses it to validate column coverage.
4. Traverses it again to form batches, looking up slots and column blocks per row.
5. Runs the kernels and applies the predicate results.
6. Rebuilds selected order, even though membership did not change.

The validation callback accounts for 16.4% inclusively; the batch-formation
callback accounts for 48.5% inclusively, including kernel work performed when
batches flush. Addition and comparison together account for 14.45% in self cost.
Each kernel runs 707 times per cycle: execution is already batched, but forming
those batches still performs substantial work per row. The two alignment passes
each visit 90,000 rows.

`selected_order_dirty` starts true whenever a full predicate rebuild is requested.
This causes the final order rebuild independently of actual membership changes.
See [filter binding](../../../include/bg/frame/frame.hpp),
[column alignment](../../../include/bg/frame/column.hpp), and
[expression evaluation](../../../src/column_expression.cpp).

### Payload-only updates remain incremental

For clustered holes, payload-only updates cost 1.13 million instructions per
cycle. The frame source accounts for 74.6% inclusively and the filter for 20.4%.
The predicate kernels and selected-order rebuild do not run. Optimization should
preserve this behavior.

## Proposed changes, in order

1. Evaluate full predicate requests directly over the source row set, using its
   native batch traversal when applicable. Keep the changed-row index for partial
   requests. Rebuild selected order only when membership or the source binding
   requires it. This removes redundant full-row work without changing public APIs.
2. Give selected-row traversal access to contiguous source spans so consumers
   reuse the filter's selection work instead of rediscovering batches. Preserve
   source order and shared row identity. Decide the representation before changing
   `RowSet`; dense and fragmented selections both need coverage.
3. Use batch validity directly during Arrow export where its representation and
   offsets permit borrowing. Skip bitmap construction for batches known to be
   all-valid. Establish validity from block metadata rather than replacing the
   existing scan with another scan.
4. Address changed-membership order maintenance. Measure retaining sequence
   capacity first, then decide whether incremental span maintenance justifies a
   new representation. Keep the source-order contract.
5. Retain explicit borrowed and owning Arrow exports. Consider a separate adaptive
   export only after batch traversal provides cheap fragmentation information.
   Borrowed export must keep its value-buffer sharing contract.

Repeat the existing native workloads after each change. Instruction profiles
identify work to remove; native timings establish its effect on elapsed time.

## Reproduction

From the frame lane, profile each binary sequentially. Use the exact outer
overload: toggling all `evaluate` overloads would also toggle nested calls.

```sh
mkdir -p build/frame-profile-2026-10-22
BG_FRAME_EVALUATE='_ZN2bg12RuntimeGraph8evaluateENSt6chrono10time_pointINS1_3_V212system_clockENS1_8durationIlSt5ratioILl1ELl1000000EEEEEEPKNS_17RuntimeGraphInputE'
valgrind --tool=callgrind --demangle=no --collect-atstart=no \
  --toggle-collect="$BG_FRAME_EVALUATE" --dump-after="$BG_FRAME_EVALUATE" \
  --callgrind-out-file=build/frame-profile-2026-10-22/expression.callgrind \
  --log-file=build/frame-profile-2026-10-22/expression.valgrind.log \
  build/tsf-row-release-arrow-off/bg_frame_expression_benchmark \
  --report build/frame-profile-2026-10-22/expression.instrumented.md \
  --csv build/frame-profile-2026-10-22/expression.instrumented.csv
```

For Arrow, use `build/tsf-row-release-arrow-on/bg_frame_arrow_benchmark` and
replace the `expression` output prefix with `arrow`. Instrumented report timings
are excluded from the results above. Raw profiles remain in the lane's ignored
`build/frame-profile-2026-10-22` directory.

Expression produces 168 evaluation dumps: 14 per case. For zero-based case `c`,
steady dumps are `14*c+4` through `14*c+14`. Arrow produces 234 dumps: 13 per case;
steady dumps are `13*c+3` through `13*c+13`. The CSV gives case order and ranges.
