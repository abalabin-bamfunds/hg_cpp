# Compaction proposal

Status: explicit compaction and its tests are implemented. Automatic scheduling
is deferred until representative BG pipelines demonstrate enough consumer
savings to justify maintenance. BG's invocation-end callback API is available;
frame storage does not register automatic maintenance.

For future automatic scheduling, mutations collect cheap statistics; storage assesses final
occupancy after mutation and plans and allocates only when justified, at the
end of the owning node invocation before downstream consumers run.
Movement can pack rows toward either end or the middle while preserving current
relative order. Algorithms retain their previous contributions; full removed
rows are not retained for them.

## Purpose

Reduce deletion gaps and Arrow chunk counts when the expected traversal savings
justify moving cells. Keep row IDs, user keys, cells, row domains, deltas, and
last-updated times unchanged. Compaction alone does not publish an update.

Compaction cannot remove gaps caused by filtering out live source rows.
Buffer boundaries also continue to produce multiple batches.

## Policy ownership

The storage owner decides whether, where, and how much to compact. Node authors
do not call maintenance after removals or choose fragmentation thresholds.
Native and computed storage coordinate their own columns:

| Output | Storage moved |
| --- | --- |
| Native TSC | Its cells and physical row mapping |
| Native TSF | All columns and their shared physical row mapping |
| Computed TSC | Its result cells and local physical row mapping |
| Computed TSF | Its computed columns and their shared local mapping |

Plain view outputs do not own cells and have no compaction operation. Computed
outputs leave borrowed columns and source traversal order unchanged. Concrete
and runtime schemas use the same storage operations.

The decision uses occupancy runs, actual buffer boundaries, owned column widths,
null storage, and allocation requirements. Evaluate candidates by both movement
cost and the batches or Arrow chunks they remove. A deletion percentage alone
does not describe either cost.

Measure native batch traversal and Arrow export separately; their costs differ.
The automatic policy, its thresholds, and any required statistics remain open
pending measurements.

Mutation only collects cheap statistics about affected buffer ranges. It does
not construct movement plans or allocate compaction buffers. Assess the final
occupancy after all mutations in the invocation: additions can refill holes
created by earlier removals. Plan and allocate only if that final state warrants
maintenance. Remember unsuccessful assessments until sufficient changes or
additional changes justify another assessment.

## Timing

Run maintenance at a boundary where no invocation holds borrowed values or
batches. Compaction can run without a logical mutation and must not reset cycle
changes or recycle removed IDs. A consumer read must not trigger movement that
invalidates another view already obtained in that invocation.

Run maintenance at the end of the owning node invocation, after successful
completion and before downstream consumers run. This timing is required to
reduce the cost of downstream traversal in the current cycle. Provide explicit
invocation-end callback registration independent of publication and subscriber
structure. Contained output storage registers through the current invocation;
collection runtimes do not recursively forward publication callbacks.

Storage registers work when collected statistics justify reassessment. The
callback checks final occupancy, then plans, allocates, and performs maintenance
only when justified. Mutation does not prepare compaction allocations. The
callback needs a defined allocation-failure contract.

Callback records use invocation scratch. Run callbacks before invocation scratch
resets; their allocations remain valid through callback execution.

A node that returns pending must not modify outputs or leave active
invocation-end callbacks. It may modify outputs on resumption only if that call
returns complete. Returning pending with active callbacks is an evaluation
error. Registrations do not need to survive suspension.

The callback API must define owner cancellation and failure cleanup. No
frame-specific policy belongs in BG core.

## Removal lifetime

Groupby and pivot nodes retain enough state to reverse each row's previous
contribution after modification or removal. Persistent algorithm state uses row
IDs; it must not retain physical slots or borrowed cell pointers.

Erasure releases cells from every column, including key columns, and frees the
physical slot. The existing removal-key API retains separate copies of key
tuples. Removed IDs and delta information also remain through the removal cycle.
Compaction must not reset those changes, release retained keys, or recycle IDs.

This retention does not pin physical slots. Compaction can complete packing of
live rows in its chosen ranges at the selected maintenance boundary. It needs
no later movement pass to finish removing deleted cells. Returning all unused
capacity remains a separate operation.

## Movement and order

Consider packing within existing buffer ranges before moving rows across buffer
boundaries. Partial maintenance can eliminate expensive gaps without packing
the complete table. Count the resulting chunks using actual column and row-ID
buffer boundaries.

For a chosen range, preserve relative row order and allow the compacted rows to
start anywhere that fits. Packing toward the beginning, end, or middle are all
candidates. With equal movement cost per row, retaining the longest existing
live run minimizes the number of rows moved to form one contiguous range:

```text
before: aaaa....BBBBBBBB....cc
after:  ....aaaaBBBBBBBBcc....
```

Leave `BBBBBBBB` in place; move the four earlier rows right and the two later
rows left. This moves six rows. Packing at either extreme moves ten or twelve
rows. Move earlier runs in descending order and later runs in ascending order.
Within each run, use overlap-safe movement toward its destination.

Keeping relative row order supports the existing rule that filters enumerate
rows in source order without rebuilding membership or selected-ID sequences.
It does not add an insertion-order guarantee: insertions still reuse holes.

Leading or trailing holes need not be filled when they create no extra live
runs. A long-lived internal gap may justify many moves if it repeatedly creates
expensive chunks. Compare that benefit rather than treating movement count
alone as the objective. Null distribution and allocation requirements can also
make the longest run a more expensive anchor than another candidate.

The storage owner coordinates movement across all columns that share a physical
mapping. `NativeRowSlots` owns the mapping transition; `NativeColumnData<T>`
owns preparing and relocating physical cells. Frame field descriptors dispatch those
operations for runtime schemas. Cell relocation does not use logical `set` or
`erase_slot`, because those operations change modification information.

Key indexes already identify rows by ID. Compaction does not rebuild them.
Removed keys retain their existing cycle lifetime.

## Cached physical mappings

Add a 64-bit layout revision to `NativeRowSlots`. Increment it before the first
committed move in a compaction call. No-op calls leave it unchanged.

A cached `SelectedOrderMapping` records the revision under which its consecutive
slot proof was established. Before emitting that proof, compare revisions:

- An unchanged revision uses the cached slot offset.
- A changed revision recomputes the proof from the block's selected IDs and
  current ID-to-slot mapping, then caches the result and revision.
- A block without a physical proof continues through the existing ID lookup
  path. Membership rebuilds can discover new proofs.

Rechecking a stale block needs no allocation. It scans that block once on its
first traversal after maintenance; subsequent traversals use the revision
check. Nested filters validate upstream proofs when consuming their spans.
Views need no logical tick for this validation. Native spans are generated
from current slots and do not need cached-proof validation.

Revision storage grows per physical mapping and cached span block, not per
cell. Normal cached-span traversal adds one revision comparison per block;
it does not add a lookup or timestamp for every row.

The revision also covers a compaction call that completes some row moves before
an allocation failure. Partial packing can split a previously contiguous run.
Preserving relative order alone is therefore insufficient to trust old proofs.

## Allocation failure

Commit one contiguous live run at a time within a common mapping and column
buffer range:

1. Prepare every owned column's destination validity capacity and packed
   Boolean bytes. Keep logical values and the mapping intact during preparation.
2. If preparation throws, leave the run at its source slots. Prepared capacity
   can remain allocated.
3. Relocate cells and validity, then commit both mapping directions and live
   occupancy. These operations must not allocate or throw.

Numeric columns relocate contiguous valid subruns within existing buffers.
Packed Boolean columns move bits in overlap-safe order after preparation.
Null cells have no numeric value object to relocate. Mapping entries are
already constructed, so mapping updates assign values rather than relocating
object lifetimes.

Previously completed runs remain committed if preparation of a later run fails. Logical
values, row order, and deltas remain unchanged; cached proofs use the revision
check above. The exception follows BG's normal evaluation failure behavior.
This avoids a second full table or a whole-table rollback mechanism.

Choose destinations within the initialized physical mapping range. Mapping
entries and occupancy capacity then already exist whether movement is left or
right. Numeric destination storage shares the allocated source block. Cell
preparation handles allocations for packed Boolean storage and validity. The
current primitive cell types permit nonthrowing movement and mapping commit.

## Capacity trimming

Compaction creates longer contiguous live ranges. Movement within an allocated
block preserves its constructed-cell count. Returning trailing capacity
can favor packing left, while reducing chunks can favor a middle range.
Include that distinction in the cost comparison. Initialized mapping prefixes,
packed Boolean bytes, bitmaps, and selected-ID buffers have separate retention
rules.

Capacity trimming is the next operation to design. It must not renumber live
IDs or discard removal-cycle information.

## Verification before integration

- Check cells, nulls, keys, IDs, order, and deltas before and after compaction;
  include packed Booleans, zero-column frames, and no-op calls.
- Exercise native, computed, and mixed borrowed/computed outputs with concrete
  and runtime schemas. Include independent physical mappings.
- Read unticked filters, nested filters, selected columns, and Arrow views
  after source compaction. Compare with an owning dense export.
- Inject allocation failures during preparation of later columns and later
  runs. Verify complete runs, unchanged logical values, safe cached proofs,
  and retry through the storage API.
- Benchmark maintenance separately from subsequent traversal and Arrow export.
  Include early holes, clustered deletions, random deletion densities, and
  scattered filters. Compare left, right, and middle packing within buffers and
  across buffers. Record rows moved, chunk counts, and allocation costs.
- Measure the break-even traversal/export count and policy overhead, including
  outputs consumed only by ID-based incremental reads.

Implemented mechanics: storage-owned manual compaction preserves row order,
supports movement in either direction, and uses revision-checked cached proofs.
The BG invocation-end callback API is available for storage registration.
Automatic scheduling remains deferred. The [benchmark](benchmarks/compaction.md)
records the maintenance and consumer costs measured so far.
