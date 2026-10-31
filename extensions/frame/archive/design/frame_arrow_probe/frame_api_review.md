# Review of the frame API proposal

Status: design review and recommendations, not approved design or implementation.
Reviewed the [I/O proposal](frame_io_api_proposal.md), the associated column and
utility proposals, and BG root revision `aef312f1`. The experiment lane is at
`17153ce2`; framework links below refer to the current root. External sources
were read on 2026-10-02. API names in this review are illustrative.

## Assessment

Keep the four types: scalar `Column<T>` and `Frame<SCHEMA>`, incremental
`TSC<T>` and `TSF<SCHEMA>`. They fit BG's distinction between a scalar value and
a time series with collection changes. Keep representation choices behind the
runtime type, and use BG's scheduler and publication rules.

The proposal is not ready to implement. The principal gaps concern observable
state, identity, and delta consumption. Arrow buffer compatibility alone cannot
resolve these. The following changes should precede implementation:

| Priority | Finding | Required action |
| --- | --- | --- |
| Correctness | Shared output cells can change while the producer cannot publish | Define invalidation, activity, source lifetime, and guarantee rules for borrowed outputs |
| Correctness | A consumer cannot detect missed changes or reference resampling | Supply publication continuity and explicit rebuild information |
| Correctness | A shared row-ID domain does not establish alignment | Separate identity, membership, and order; define computed `TSC` membership |
| Correctness | A producer could inherit an unsuitable input runtime | Select and validate output storage from producer capabilities |
| Semantics | IDs-only removals move substantial state into consumers | Compare consumer caches with cycle-scoped old-value storage before deciding |
| Semantics | Ordered results are not completely described by the proposed row delta | Define filter order now; keep sorted-result publication explicitly unresolved |
| Performance | Per-row callbacks and logical positions dominate the proposed API | Add batch access and direct ID-based reads before implementation |
| Integration | New time series need more than a runtime resolver entry | Cover references, children, visitors, guarantees, and schema matching |

## What to borrow

| System | Useful design | Application to BG | Boundary |
| --- | --- | --- | --- |
| Arrow [A1, A2] | Typed column buffers, independent null validity, slices, dictionaries, batch kernels | Interchange and compatible batch execution; retain multiple BG runtime layouts | Arrow's array format does not define BG row identity or table mutations |
| Polars [P1, P2] | Expression planning, projection and predicate pushdown, common subplan elimination, batch execution | Resolve expressions at wiring; process only required columns; fuse compatible expressions within frame nodes | Streaming query execution in batches does not supply persistent insert/update/delete maintenance for BG |
| kdb+ [K1, K2] | Tables built from column lists; optional keyed tables; column operations and keyed upsert | Keep user keys optional; put keyed ingestion and indexes in nodes; support column batches | A q table's row position is insufficient as stable identity after BG deletion or compaction |
| Perspective [V1, V2] | Mutable tables, live configured views, indexed partial updates, Arrow row-update callbacks, pivot column paths | Separate stored data from view configuration; share source columns; distinguish omitted updates from null values | Its documented update callback is not a complete specification of arbitrary BG derived-view deletions or identity |
| Deephaven [D1] | Row sets, shared column sources, column redirection, modified column sets, cycle-scoped previous values | Closest comparison for incremental TSF nodes; use its lifecycle and state costs to test our contracts | Its ordered row keys and shift protocol need not become BG row IDs or an in-place reorder API |
| DuckDB [U1] | Flat, constant, dictionary, and sequence vectors with a common access interface | Let kernels read selected and encoded batches without immediately gathering them | Its dictionary selection vector is an execution representation; it is not an Arrow table row-selection feature |
| Feldera / DBSP [F1, F2] | Stateful incremental query evaluation and explicit insert/delete changes | Use delta algebra and retained indexes to derive aggregation and join nodes | Translating a BG modification into a retraction requires the old contribution |

### Perspective in particular

Perspective's `TableInitOptions.index` chooses insert-or-update behavior by
column value. Its optional `limit` overwrites the earliest inserted rows.
`Table::update` accepts partial column updates for indexed tables, while the
table schema remains fixed. `View` keeps its query configuration fixed while
incrementally updating its data. These are useful separations for BG [V1, V2].

BG should keep business-key lookup outside internal row identity. A bounded
table should express eviction as a removal followed by a distinct insertion;
reusing a physical slot must not silently preserve the evicted row's identity.

Perspective's `group_by`, `split_by`, and `column_paths` also support the
earlier proposal for structured pivot column names. A dynamic collection of
whole columns still needs column addition/removal and schema publication rules.
That can remain a later extension to the fixed-schema design.

The inspected Perspective source documents Arrow updated-row callbacks and a
separate removal callback carrying source index values. The latter describes
indexed-table remove, replace, and clear operations. It does not establish a
general protocol for rows leaving a filtered or pivoted BG result. Adopt the
table/view separation; specify BG result deltas independently.

### kdb+ column operations and maintained indexes

kdb+'s table representation makes whole-column expressions natural, and its
sorted, unique, parted, and grouped attributes describe useful data properties
or support lookup [K1]. For BG, the useful application is to retain a join or
group index across ticks and update only affected entries. Ordered time data
can support specialized searches when the operator maintains that ordering.

Such properties need an owner that maintains or invalidates them after edits.
An Arrow layout or a row ID does not prove sortedness or uniqueness. Keep this
information with an operator's index or runtime representation until a concrete
operator needs a public capability. Interned symbols suit repeated categorical
keys; arbitrary strings should remain a separate value/storage choice.

## 1. Borrowed outputs must obey publication

Invocation-scoped reads solve the consumer's pointer lifetime. A selected
output still has state between invocations. Its current value must remain
consistent with its last publication, including when the node cannot evaluate.

Consider `filter(source, predicate)`:

1. The output includes row `r`, whose value is 10.
2. In the next cycle, `source` changes that cell to 20.
3. A required predicate input becomes invalid, so the filter cannot evaluate.
4. Preserving a source pointer plus the old mask exposes 20 without an output
   update. Another downstream input can schedule a consumer that observes it.

The TSS/TSD default of preserving the output therefore cannot be adopted
unchanged for a borrowed selected frame. Keeping the source allocation alive
does not keep its published value unchanged. A removable inner graph creates
the additional risk that the source disappears at the cycle boundary.

**Recommendation:** make ordinary and deduplicated `TSC`/`TSF` invalidatable.
First publication validates them; `clear()` publishes a valid empty value.
For a borrowed producer, every mutable source of cells or membership must be
an active, tracked dependency. Required-input invalidation invalidates the
output and its column views; revalidation rebuilds them. This is a new
recommendation, replacing the tentative TSS/TSD validity precedent.

Validate these constraints at wiring/runtime binding boundaries. A borrowed
output cannot support passive sampling, deliberate missed source updates, or
`preserve_output` merely by retaining a reference. Those behaviors require
stable owned backing or a proven immutable source. Copying at a late
invalidation callback may already be too late: the source may have overwritten
the previous value earlier in that cycle.

Apply the same reasoning to guarantees. A `final` or `const` output cannot
continue borrowing changing cells after publication. A deduplicated selected
output needs proof from its sources or value comparisons supported by retained
state. Runtime representation choice must not weaken the declared guarantee.

This follows BG's [evaluation contract](../../../../docs/evaluation.md) and
[validity contract](../../../../docs/time-series-validity.md). Selected frames
are distinct logical outputs with their own membership and change record;
column projections of one TSF expose that parent's authoritative child state.

## 2. Delta continuity and reference sampling need an API

The proposal says that new consumers initialize from the whole value. It does
not explain how a node recognizes reference rebinding or a gap in observations.
`modified()` and a row domain are insufficient.

Example: a sum consumes a source at cycle 1. A different required input is
invalid at cycle 2, when the source deletes a row. The sum resumes at cycle 3,
when the source modifies another row. Applying only cycle 3's delta retains the
deleted contribution. This can happen even with an active source input.

**Recommendation:** expose an opaque current publication token and the base
token to which the current delta applies. Tokens include logical-series
incarnation; bindings must also expose resampling. They need not be counters
if BG's time and reference state supply equivalent information. This is
constant metadata per series, with sampling state on its binding and a retained
token per consuming input. Also
report whether that delta is still available in the current cycle. Once its
change records are cleared, a matching base token alone cannot authorize
incremental consumption.

The consumer rule is:

```text
invalid input                      -> apply the node's invalidation behavior
uninitialized or resampled          -> rebuild from current rows and values
same observed publication           -> no value work
unavailable delta or base mismatch  -> rebuild from current rows and values
available delta with matching base  -> apply the current row delta
```

On rebuild, discard or reconcile old consumer state before loading the current
value. Rebinding between two selections of the same source needs this rule
even though both selections use the same row-ID domain. An accepted mutation
with empty final row sets can still advance publication under BG's rules.

Integrate this with BG's
[reference sampling](../../../../docs/time-series-references.md#input-sampling).
Do not make nodes infer target changes from data pointers, and do not imply
that deltas accumulate until a passive consumer reads them. A rebuild marker
is a proposed extension for frame inputs, not an existing BG facility.

## 3. Identity, membership, order, and lookup are separate

The same domain means that equal IDs identify the same source row. Two filters
of that source can have different memberships, lengths, and orders. A domain
check alone cannot permit positional column arithmetic.

**Recommendation:** let frame-library nodes share an owned description of
current row membership and order. Expose it through invocation-scoped views.
The exact C++ type remains to be proposed. Support these relationships:

| Producer | Identity | Membership and order |
| --- | --- | --- |
| Owning TSF / standalone owning TSC | Allocates a domain and live IDs | Owns them |
| Fixed-column projection | Preserves parent IDs | Shares parent rows |
| Row-preserving expression TSC | Preserves input IDs | Follows the input rows; owns computed cells |
| Filter | Preserves source IDs | Owns a subset in source order |
| Group or general join | Allocates output identities | Maintains group or match membership |

The expression writer must support values for inherited IDs. `add_row(cell)`
with newly allocated IDs cannot implement `frame["bid"] + frame["ask"]` as
an aligned TSC. A parent-column writer can edit cells but cannot add or remove
rows. Those are producer capabilities, not different consumer graph types.

For positional kernels, prove the same current ID sequence. Sharing the same
membership/order object is a cheap proof. Otherwise perform explicit
alignment or reject the combination. Scalar `Column` kernels still align by
position; the enclosing TSC node supplies this proof.

Read current cells and batches directly by ID as well as by logical position.
`position_of(id)` on a sparse selected frame requires a rank operation; forcing
every incremental access through it adds avoidable work. Dense positions and
physical locations are invocation-scoped. Persistent node state uses IDs.

For an initial implementation, recycled IDs can be valid if reuse waits until
removal cleanup and consumers rebuild after gaps. An excluded filter row may
re-enter with the same source identity while that source row remains live.
Source deletion ends that identity. If IDs may survive removal or identify rows
outside this consumption discipline, add generations or another incarnation
mechanism. Generation numbers detect stale IDs; they do not repair missed
deltas. ID width remains a capacity decision, not an Arrow requirement.

The proposed two 32-bit mapping arrays cost about 8 MB per million capacity
slots. Separate logical order, generation storage, selected-row rank data, and
consumer caches are additional costs. A tiny filtered result should not be
forced to allocate maps proportional to the source's maximum ID.

## 4. Removed values are a material state-cost decision

An IDs-only removal contract works, including through a pure selected view.
It should remain the candidate contract, with explicit consumer costs.

| Node | State needed with current changed values and removed IDs |
| --- | --- |
| Count rows | Prior count, plus continuity and any needed membership accounting |
| Filter | Output membership and predicate dependencies |
| Sum a mutable column | Prior contributing value/null state by live row ID |
| Grouped sum | Prior group key and contribution by row ID, plus group totals |
| Minimum/maximum | Enough indexed values or counts to replace a removed extremum |
| Join | Match indexes and prior join keys; additional projected data as required |

A deletion-only sum otherwise needs only a running total when removal supplies
the old cell. With IDs alone it can need O(N) cached values. Multiple consumers
can duplicate that cache. The earlier assertion that consumers already need
the same state for modifications does not cover append/remove workloads or
the aggregate cost across many consumers.

Deephaven requires prior values for both modifications and removals, valid only
within the update phase [D1]. A BG source could retain the first old value for
each overwritten cell in a cycle, plus removed cells. This would cost in
proportion to changed payload for that cycle rather than a permanent matrix of
per-cell timestamps. Derived selections would also need previous membership
and source access. That is substantial machinery and a change to BG's normal
modification contract; it is not free retention supplied by Arrow.

**Recommendation:** compare IDs-only consumer caches with projected,
cycle-scoped old-value retention before fixing the contract. Measure one and
many consumers, sparse updates, append/remove windows, and large replacements.
The public minimum must be uniform across materialized and selected runtimes.
Keep mandatory previous values undecided until that comparison. No removal
callback may present the source's newly overwritten cell as the old output cell.

## 5. Row deltas need precise semantics and efficient representations

Keep combined changed-live IDs and removed-absent IDs at the consumer interface.
Keep enough internal membership information to distinguish actual row entry
from a cell edit. Derived runtimes and batch initialization need that distinction
even if node authors use an upsert-style changed callback.

For a completed publication, require:

- Changed and removed sets are disjoint and contain each ID at most once.
- Every removed ID was in the preceding published membership. A row inserted
  and erased before publication produces no removal for consumers.
- Every newly visible row supplies all current cells, including null cells.
- Changes to an existing row identify affected columns. An omitted column
  update is different from setting that column to null.
- A row removed and reintroduced with the same still-live source identity
  during one invocation is interpreted relative to the preceding publication.
  Reuse for a different source row cannot collapse into that operation.
- Column children share row additions/removals. Cell modifications belong to
  the affected children, with their own publication times.

Deephaven communicates a modified row set and a modified column set [D1].
This inexpensive combination can conservatively describe a rectangle of cells.
For BG, the current promise that a child changes only when that column is
written is stronger: sparse edits to different columns need exact per-column
ID subsets or an equivalent representation. Do not quietly substitute the
row/column cross product for that promise.

Use range/all-row forms for uniform batch changes and sparse forms for scattered
edits. These are representations of one delta contract. Membership additions
must not create a duplicated ID list for every column. Dense updates must not
require a separately allocated timestamp or hash-set entry for every cell.

## 6. Ordering constrains compaction and sorted outputs

**Recommendation:** owning writers append new rows in insertion order and
preserve the relative order of survivors. Filters preserve source order;
row-preserving projections and expressions preserve their input order.
Compaction changes no observable order and publishes no row change.

This has a physical cost. Moving the last physical row into a hole needs an
order mapping to preserve these semantics. Once physical order differs from
logical order, a dense physical buffer may still require a gather for Arrow
export. Stable compaction costs more movement but can restore long logical runs.
Defragmentation must optimize the order actually read, not only occupancy.

An existing source row newly entering a filter may appear in the middle of the
result. Delta consumers must be able to obtain its final position or read the
current ordered membership. ID-based aggregate nodes need neither operation.

`TSF` still has no in-place reorder operation. A sort node's output contract
remains separate work: changing a sort key can change relative order among
already-live result rows. The current row-delta proposal must not claim to
fully express that result. A complete scalar frame result is expressible now;
an efficient incremental sorted result needs an explicit order contract.

## 7. Batch access belongs in the first API

The framework should support both small ID deltas and bulk work. Rebuilding,
initial import, expression evaluation, and large ticks already require the
bulk path. Waiting for per-row dispatch to become a measured bottleneck risks
fixing the public API around that bottleneck.

**Recommendation:** define batch interfaces before implementation, with
single-row calls as conveniences. A read batch needs:

- logical length and either a position range or associated row IDs;
- typed values and null validity;
- enough encoding information for direct, selected, constant, Boolean, or
  dictionary access, with a documented gather/decode fallback;
- invocation scope and an explicit lifetime for temporary decoded storage.

DuckDB's unified vector access is a useful example [U1]. Resolve dispatch per
batch/runtime, then run typed loops. Keep selections available to compatible
kernels instead of creating an Arrow chunk for every short live run. Compose
selection mappings so chains of filters do not require a pointer traversal
through every upstream filter for each cell. This is a desired execution
property; the representation needs measurement.

The owned writer needs complete-row batch insertion and column updates by ID.
The row-preserving expression writer needs batch writes for inherited IDs.
The selected writer needs source binding and membership changes. All publish
through the same BG completion boundary and apply the same delta rules.

Null-aware indexed fallback also needs care: an Arrow bit-packed Boolean has
no addressable BG `bool` object. Returning only `optional<Scalar<T>>` is not
yet a sufficient specification for every layout. Define typed reads or
callback-scoped decoded cell access without fabricating long-lived references.

Keep `Column` distinct from today's `List`: nulls and read-only bulk access are
a concrete semantic/API reason. Shared bulk machinery can later serve lists.
Read-only access does not remove scalar copy/equality obligations: declare
element-dependent equality support where `TS<Column, deduplicated>` requires it,
or reject that guarantee during wiring. Hashing need not be universal.

## 8. Framework integration must be explicit

BG already separates semantic and runtime types. Preserve that separation,
including Arrow-backed scalar values and selected time series layouts.
The following integration points need a focused proposal:

| BG facility | Work required |
| --- | --- |
| Scalar providers and dispatch | Register Column/Frame semantics, copy/ownership behavior, equality capabilities, runtime input compatibility, and batch dispatch |
| Schema model | Reuse named field declarations and typed field access; define Frame matching and SCHEMA resolution explicitly |
| Time series type registration | Support guarantee resolution, invalidation, matching, common types, diagnostics, and the runtime resolver |
| Input/output visitors | Handle TSF/TSC through a deliberate extension path; current dispatch enumerates built-in families |
| Collection items | Expose fixed TSF columns as TSC children; BG item IDs identify columns, while frame row IDs belong to the extension |
| Publication and subscriptions | Publish affected children; keep root and child deltas coherent; support dynamic column references |
| Reference representations | Support rebinding across runtime layouts, resampling, invalidation, and removal cleanup |
| Output construction | Select a layout compatible with the producer's writer, independent of compatible input layouts |
| Runtime lifecycle | Use BG allocation, initialization, finalization, and deferred cleanup; release selections before source storage |

The current compiler's `find_output_runtime_type` passes input runtime types to
`resolve_time_series_runtime_type`, which can select the first exact semantic
match. A materializing frame node fed by a selected frame could therefore inherit
selected storage unless its output runtime is explicitly supplied. Producer
capability validation is a correctness requirement, not just an optimization.

The current `Out` has one template argument. Start with internal writer support
for owning, row-preserving, and selected producers, then settle a public
output-only capability declaration for third-party nodes. Do not expose cell
mutation methods that fail merely because the producer received a selected
runtime. All consumers continue to accept `In<TSF<SCHEMA>>` or `In<TSC<T>>`.

Use BG's existing schema field syntax where possible. A required frame column
containing null cells is different from an optional Bundle field or an optional
node input. Full-row construction, partial cell updates, and patch omission
must not share an ambiguous optional value. Fixed-schema matching should be
exact initially; broader schema projection should be an explicit operation
until safe read/write compatibility has been specified.

The relevant implementation boundaries are
[type visitation](../../../../include/bg/type/type.hpp),
[input visitation](../../../../include/bg/time_series/time_series_input.hpp),
[runtime resolution](../../../../src/time_series/time_series_runtime_type.cpp),
[output layout selection](../../../../src/compiled_graph.cpp), and
[TSB field access](../../../../include/bg/time_series/tsb/tsb_input.hpp).
Typed Column/Frame scalar extensions can use existing provider facilities;
fully generic TSF/TSC integration needs framework work. An extension library
should not own a second scheduler.

## Operator checks

These cases should be worked through before freezing the API:

| Operation | Incremental rule | Required contract |
| --- | --- | --- |
| Project columns | Forward selected columns and relevant changes | Shared membership; child publication; borrowed-output lifecycle |
| `bid + ask` | Union the input column changes, compute current results | Proven row alignment; inherited IDs; null propagation |
| Filter | Process source membership, predicate changes, and selected payload changes | Source-ordered subset; removed IDs; active dependencies |
| Grouped sum | Retract the prior contribution, apply the current one | Cached or supplied old group/value; group identities |
| Join | Update match indexes and emit affected match changes | Both completed input states; old keys; output identities for matches |
| Snapshot | Publish a stable complete scalar | Immutable ownership or copying before subsequent mutation |

For a filter, recommend null predicate cells mean excluded. A row newly
included supplies every projected column. An already included row forwards
only affected projected columns. A change to an excluded payload alone does
not tick the result. Initialization or a continuity gap takes the full path.

For an equijoin with old inputs `L`, `R` and signed row changes `dL`, `dR`, the
result change is:

```text
dJ = dL join R + L join dR + dL join dR
```

Equivalently process `dL` against old `R`, then `dR` against updated `L`.
Processing each delta against both updated sides double-counts cross changes.
BG's completed-predecessor rule supplies consistent current inputs; the node
still needs retained state to implement this algebra. Equal-valued duplicate
rows remain distinct through row or match identity. Outer joins need additional
unmatched-row bookkeeping.

For floating-point sums, incremental subtraction/addition can differ from a
full sum due to rounding. Specify numeric tolerance or a stronger accumulation
policy in the operator tests. Null transitions must update both contribution
and non-null count; an all-null result is distinct from an invalid TS.

## Performance and Arrow boundary

Optimize small ticks for affected rows/columns and operator state. Use batch
kernels for initialization, rebuilds, and large changes. Zero-copy describes
cell movement; it does not bound selection maintenance, chunk creation, rank
lookup, predicate evaluation, or downstream scan cost.

The existing probe measures fixed-width export and Arrow compute. It does not
measure a native TSF delta pipeline, Polars, multiple consumers, or mutation
maintenance. Its evidence supports choosing export by fragmentation and work,
not a universal percentage of deleted rows. A zero-copy export with thousands
of small chunks can lose to one allocation and gather.

Immutable Arrow arrays can share buffers through slices; immutable publication
does not itself imply copying every cell. The expensive operation is acquiring
a stable new value after edits when the chosen representation cannot share
unchanged storage. Keep that cost separate from wrapper allocation and kernel
execution.

Polars' documented optimizations operate on query plans [P1]. A claimed 100x
advantage must identify workload, plan, conversion, and allocation costs.
BG should borrow expression planning and column pruning, then compare complete
equivalent pipelines. O(changed rows) maintenance can dominate the choice of
an O(all rows) scan backend, subject to output size and operator fanout.

Arrow export must distinguish shared value buffers from copied descriptors,
validity, decoding, and gathering. Boolean packing, offsets, alignment, and
dictionary domains require type-specific adapters [A1]. Arrays backed by
mutable BG storage are borrowed only for the invocation; release callbacks or
an Arrow wrapper do not freeze those cells. Retained scalar/Python values need
stable ownership. This does not add a retention requirement to ordinary BG reads.

Symbols are good candidates for dictionary encoding, as column systems
demonstrate, but BG symbol handles are not automatically Arrow dictionary
indices. Specify dictionary identity and code translation; compare values
across differing dictionaries. General strings need a bytes/UTF-8 policy and
an adapter for the selected Arrow string representation. Null, empty string,
and empty symbol must have deliberate semantics. kdb+'s type-specific null
representations [K3] should not replace Arrow-style validity in BG columns.

An external delta value eventually needs an identity domain/incarnation, base
and result publication tokens, removed IDs, and changed values with explicit
column presence. Complete replacement/rebuild must be expressible. Arrow can
carry the column payloads; Flight does not define these table delta semantics.
Deephaven's Barrage is an example of adding update metadata to Arrow Flight
[D1]. Transport mechanics remain outside this proposal.

## Validation before implementation approval

No new code or benchmark was run for this review. The next experiment should
test semantic contracts before expanding the storage utilities:

1. An owning source, a row-preserving expression TSC, a selected TSF, and a sum
   consume scripted ticks. Compare current values and deltas with full
   recomputation, including nulls, duplicates, and same-cycle write sequences.
2. Exercise first observation, skipped cycles, passive reads, invalidation,
   rebinding between selections of one domain, and source removal in a nested
   graph. Reject unsupported borrowed-output configurations explicitly.
3. Compact storage and reuse physical slots. Check that IDs, logical order,
   current values, and change records remain correct.
4. Feed selected and Arrow-backed inputs to materialized writers. Verify output
   runtime selection, generic visitation, child subscriptions, and guarantees.
5. Measure complete pipelines at several tick sizes and fragmentation patterns.
   Include initialization, mutation, state maintenance, publication, allocation,
   compaction, and export when used. Compare IDs-only caches with old-value
   retention for one and many consumers; report steady and peak memory.

These are proposed gates, not completed tests. Dynamic pivot schemas, transport,
and incremental sorted-result publication can remain later work. Borrowed-output
lifecycle, continuity, alignment, producer capabilities, and the batch interface
must be settled first.

## Sources

- **A1:** [Arrow columnar format](https://arrow.apache.org/docs/format/Columnar.html),
  including validity, physical layouts, dictionaries, and IPC.
- **A2:** [Arrow C++ compute](https://arrow.apache.org/docs/cpp/compute.html).
- **P1:** [Polars lazy optimizations](https://docs.pola.rs/user-guide/lazy/optimizations/).
- **P2:** [Polars streaming execution](https://docs.pola.rs/user-guide/concepts/streaming/).
- **K1:** [kdb+ tables](https://code.kx.com/q4m3/8_Tables/).
- **K2:** [kdb+ upsert](https://code.kx.com/q/ref/upsert/).
- **K3:** [q datatypes and null values](https://code.kx.com/q/basics/datatypes/).
- **V1:** [Perspective Table API source](https://github.com/finos/perspective/blob/master/rust/perspective-client/src/rust/table.rs),
  especially `TableInitOptions`, `update`, `remove`, and `replace`.
- **V2:** [Perspective View API source](https://github.com/finos/perspective/blob/master/rust/perspective-client/src/rust/view.rs),
  especially live-view documentation, `column_paths`, `on_update`, and `on_remove`.
  These are development-branch sources, not a pinned release contract; the guide
  host was unavailable during this review.
- **D1:** [Deephaven incremental update model](https://deephaven.io/core/docs/conceptual/table-update-model/).
- **U1:** [DuckDB execution format](https://duckdb.org/docs/current/internals/vector.html).
- **F1:** [DBSP overview](https://docs.rs/dbsp/latest/dbsp/).
- **F2:** [Feldera JSON changes](https://docs.feldera.com/formats/json/).
