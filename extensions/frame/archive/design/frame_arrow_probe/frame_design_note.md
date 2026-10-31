# Frame extension working note

Status: discussion and experiment, not an approved BG design. `frame` is a
working name. This note records the constraints and questions that should
survive the Arrow probe. The [README](README.md) describes the probe, and
[benchmark results](benchmark_results.md) contain its measurements. A separate
[utility API proposal](utility_api_proposal.md) specifies the proposed changes
to `sparse_bunch`, `occupancy_index`, and `index<>`. The
[column kernel type proposal](column_kernel_type_proposal.md) examines typed
batch values and their relationship to `List<T>`.
The [framework and comparative review](frame_api_review.md) evaluates these
proposals. Its recommendations on borrowed-output invalidation, delta
continuity, shared membership, and batch APIs remain under discussion.
The [implementation plan](frame_implementation_plan.md) identifies the first
working graph and a proposed sequence of implementation slices. The sections
below record the current direction; API spellings and storage details remain
proposals.

## Purpose and agreed constraints

- Build a library of frame-manipulating BG nodes. A frame is conceptually a
  columnar representation of a time series with structured row values. Rows
  can be added, removed, and modified without copying the complete frame on
  each tick.
- Support frames with and without user keys. A time series dictionary (`TSD`)
  is a useful comparison, but its key and `getitem` semantics do not settle the
  frame interface. Row selection and column selection still need distinct
  operator definitions.
- Nodes can observe a whole-frame view or a row delta during an invocation.
  Neither view has a lifetime guarantee after that invocation. No design
  should require a consumer to retain a pointer into a frame across cycles.
- The frame library is the primary consumer of row identity. Its nodes must
  support incremental operations such as filtering, sorting, grouping, joins,
  and pivoting. User-facing row handles are not a current requirement.
- Previous cells are available through consumer opt-in at their column's
  storage owner. The default delta carries current changed rows and removed
  IDs. A view combines its previous membership with the owner's previous
  cells when a consumer requests them.

## Scalar values and time series

The following names are provisional. They separate current values from change
tracking:

| Scalar value | Time series with that current value |
| --- | --- |
| `Column<T>`: ordered, nullable cells of type `T` | `TSC<T>`: current column, row domain, and row changes |
| `Frame<SCHEMA>`: aligned columns and row order | `TSF<SCHEMA>`: current frame, row domain, and row changes |

`Frame<SCHEMA>` describes one complete table value. It can be borrowed or
owned. Neither scalar type contains per-cycle change information. Internal row
IDs belong to `TSC` and `TSF`; a scalar frame presents rows in logical order.
All scalar runtime types for the same `Frame<SCHEMA>` must present the same
schema, row count and order, cell values, and nulls. Physical chunks and row
slots are not part of the scalar value.

`In<TSC<T>>::value_view()` would borrow a `Scalar<Column<T>>`, and
`In<TSF<SCHEMA>>::value_view()` would borrow a `Scalar<Frame<SCHEMA>>`.
Constructing either view must not copy cells. The owner retains the column
buffers and row-order data. These borrowed views remain valid only during the
node invocation.

`TS<Column<T>>` and `TS<Frame<SCHEMA>>` publish complete scalar values. Their
inputs expose read-only views; an output replaces its value to publish a new
one. `TSC<T>` and `TSF<SCHEMA>` instead mutate their current storage through
their outputs and publish row changes. This follows the distinction between
`TSS<T>` and a scalar set held in `TS<Set<T>>`.

Selecting a column from a scalar frame gives a scalar column. Selecting it
from `TS<Frame<SCHEMA>>` produces `TS<Column<T>>` through a projection node;
replacement of the whole frame gives no row-change information. Selecting it
from `TSF<SCHEMA>` can expose a `TSC<T>` child with row changes.

`TSF<SCHEMA>` owns its current membership and change record. A materialized
runtime owns column buffers; a selected-view runtime retains a source and
stores its selection.
`In<TSF<SCHEMA>>` exposes its current frame and row delta;
`Out<TSF<SCHEMA>>` applies row insertions, updates, and removals. For a fixed
schema, `frame["column"]` selects a `TSC<T>` child with its own last-updated
time. The child shares the frame's row domain, buffers, and row addition and
removal records. A cell write records its row ID for the affected column
during the current cycle. The frame-wide delta combines row additions,
removals, and modifications. No row-by-column timestamp matrix is required.
Dynamic column schemas remain open.

At publication, row liveness, order, values, null validity, row IDs, change
records, and child update times must be consistent. Subscribers observe the
completed state after the node finishes. A borrowed scalar view cannot be
used across a mutation that moves or replaces its buffers, even during the
same invocation.

A `TS<Frame<SCHEMA>>` snapshot made from a live `TSF<SCHEMA>` must keep its
value stable after the source mutates. Owning construction copies mutable
cells when the snapshot is requested. An already immutable owner can retain
its buffers. This keeps snapshot costs out of ordinary row writes. The same
rule applies to `TS<Column<T>>` snapshots, including ownership requested
through erased `ScalarValue<>` handles. The
[column kernel type proposal](column_kernel_type_proposal.md)
develops the scalar and incremental kernel contracts.

## Row changes and identity

Frame-library nodes use opaque IDs with a defined row domain. IDs remain
stable across cycles and physical moves while a row exists. Filters,
projections, and row-preserving computed columns preserve source IDs. Shared
IDs alone do not prove equal membership and order.

Row IDs are internal identities, separate from optional user-defined key or
index columns. A frame need not have a user key. Modifying a key column does
not change the row's identity. Key lookup and uniqueness belong to the nodes
that provide those operations.

Each column reports its changed live IDs; removal identifies an absent row.
Additions and modifications can share the public changed-live query. Storage
keeps membership additions/removals separately so frame columns can share them.
There is one change time per column and sparse per-cycle change information.

A node can retain an ID and its own derived state across cycles. A filter
retains membership. A node that needs previous cells uses the subscription
described below. IDs identify rows independently of the lifetime of any cell
buffer or borrowed view.

Use 32-bit IDs within each row domain. Recycle an ID only after its removal
cycle. Before the first mutation at a later evaluation time, the owner clears
the preceding change record and makes retired IDs available for allocation.
This follows the existing `TSS` and `TSD` change-record lifecycle. Nodes must
discard removed IDs and rebuild their state after missed updates. A numeric
ID alone does not identify a row across domains or after removal.

The initial native layout uses an ID-to-physical-slot array and a
physical-slot-to-ID array. With 32-bit entries, the two arrays cost about eight
bytes per row of capacity, excluding occupancy, columns, and node state.
Recycling bounds the ID space by peak live and pending-retirement rows,
including additions canceled during the current cycle.

## Previous cells by owner subscription

A `ColumnSource` identifies the physical owner of its cells. A consumer
registers previous-value demand directly with that owner for the required
column. Filters and projections share that source; no requirement traversal
through graph nodes is needed. A computed column is its own storage owner.
Requesting its previous results does not request previous inputs.

The proposed owner state is:

```text
ColumnState {
    current cells
    changed IDs
    previous-value subscription count
    optional cycle-scoped previous cells
}
```

While demand is active, the owner captures the first previous cell before
overwriting or removing a row that existed in the preceding publication.
Later writes preserve that captured cell. A previous read uses the capture
when present and the current cell when unchanged. Added rows have no previous
cell; a previous null cell is distinct from an absent previous row.

Zero-demand columns allocate no previous-cell storage. Consumers share one
capture per changed cell. Batch writers can select capture behaviour once
per column batch; the concrete journal and first-write lookup remain to be
designed and measured.

A selected view supplies previous membership. A row leaving a filter exposes
its preceding output value under an active subscription, including when the
source changed the cell that caused it to leave. A filter usually needs only
membership to maintain its selection. An aggregate downstream may request
previous values of entirely different columns.

Subscriptions must be effective before the writes they cover. Rebinding
replaces the subscription to the owner. Newly attached consumers initialize
from the current value. Captured cells remain available through the cycle's
readers. The precise declaration and activation/release APIs remain open;
access must require the consumer's own subscription.

## Expressions and wiring

Column arithmetic and comparisons build deferred expression wiring records.
For a `size` column, `q.size > 10` has the semantic wiring type
`wp<TSC<bool>>`; it need not allocate a Boolean column. Operators record typed
operands, constants, null behaviour, and row dependencies.

Use BG's existing wiring-record materialisation mechanism when an ordinary
node needs a concrete `TSC`. Plan batch execution at that boundary and reuse
compatible existing materialisations. A variable assignment alone does not
force materialisation. Expression structure must retain shared subexpressions
and their source bindings.

Frame operators may consume that deferred representation while wiring their
nodes. The runtime scalar and node signatures below are provisional:

```text
Expression<SCHEMA, RESULT> {
    shared immutable expression plan
    owned or retained bound scalar parameters
}

expression_node(TS scalar inputs...) -> TS<Expression<SCHEMA, RESULT>>

filter(TSF<SCHEMA>, TS<Expression<SCHEMA, bool>>) -> TSF<SCHEMA>
```

`Expression` is a scalar carried by ordinary `TS`; it is not another
incremental time-series category. Its plan describes column reads and the
result calculation. Its parameter values must remain stable as scalar values.
The plan must keep column dependencies available to wiring; a callback that
hides graph inputs is insufficient.

For `filter(q, q.size > min_size)`, where `min_size` is `TS<int>`, wiring
creates a separate expression-construction node. It publishes a bound
expression when the threshold changes. The filter receives that expression
as one ordinary input. A constant predicate expression uses `const_`.

This division simplifies node code: expression construction handles arbitrary
scalar input signatures, while filter applies a ready expression and maintains
membership. The tree and kernel plan can be prepared once during wiring;
changing a threshold updates bound values. A shared expression executor serves
filters and computed columns.

Scalar time-series parameters broadcast over the expression's rows without
constructing repeated-value columns. For a row-wise predicate:

| Change | Predicate evaluation |
| --- | --- |
| Initial evaluation or predicate expression tick | All current rows |
| Input row addition | Added rows |
| Referenced column modification | Corresponding rows |
| Unrelated column modification | None; selected payload changes still propagate |
| Input row removal | Remove selection membership |

True includes a row; false or null excludes it. A threshold tick can scan all
rows while publishing only membership differences. An aligned materialised
`TSC<bool>` remains a useful filter input with its own row deltas.

The filter wiring implementation must create the expression-construction
node before ordinary node-input materialisation. It must not ask the existing
materialiser to return `TS<Expression<...>>` for a `TSC<bool>` record: those
are different semantic types. The exact wiring adapter is part of the first
expression implementation.

### Planning scope

Dependency tracking is per column, affected work is per row, and execution
uses batches. Retaining an intermediate across cycles, sharing it within one
invocation, and combining arithmetic into one loop are separate decisions.
An expression node may use existing kernels with reusable batch scratch;
arbitrary single-loop compilation is not required by the expression API.

For `(bid + ask) / 2`, both operations have the same dynamic column inputs.
For `a*b + c*d`, branches can change independently. Their different dependency
sets identify potential retained intermediates; they do not require separate
nodes. An intermediate required as a concrete TSC elsewhere should be reused
when planning downstream expressions.

The existing materialisation cache reuses already materialised records. If a
larger expression is materialised first, later materialisation of its child
does not automatically revise the earlier calculation. Sharing independent
of wiring-call order needs a lowering decision; its implementation remains
open. Separate expression construction is the chosen code organisation and
does not depend on that optimization.

Polars supplies the [expression/context model](https://docs.pola.rs/user-guide/concepts/expressions-and-contexts/),
[shared-expression analysis](https://docs.pola.rs/api/python/stable/reference/lazyframe/api/polars.LazyFrame.collect.html),
and operation properties such as
[elementwise evaluation](https://docs.pola.rs/api/python/stable/reference/expressions/api/polars.Expr.map_batches.html).
BG additionally needs
affected-row rules and state retained across cycles. An aggregate such as
`mean(bid)` can affect every row of `bid / mean(bid)`; input column sets alone
cannot determine the affected rows for all expressions. The first expression
set is row-wise arithmetic and predicates. Polars-style unbound column-name
templates, general aggregation expressions, and automatic fusion are later
work.

## Physical storage and order

`sparse_bunch<T>` can hold a typed column at physical positions;
`occupancy_index` can mark live rows; `index<>` can mark valid cells. Row
liveness and cell validity are separate: a deleted row does not appear in an
Arrow table, while a null cell remains in its row.

Separating row IDs from physical positions would permit compaction. The table
could move live rows into holes, move every column and validity state in step,
and update its ID mappings without reporting a row change. Compaction must
run when no node invocation is using a view. A trigger should account for the
cost of moving rows and the cost of repeated fragmented reads; no threshold
has been chosen.

The scalar frame exposes rows in logical order. `TSF` changes rows by adding,
removing, or modifying them; it has no in-place reorder operation. The initial
native writer reuses free physical slots. Insertion order is not guaranteed
after removals; compaction may change row positions while preserving row IDs
and column alignment.
A sort node produces its own ordered result without reordering its input.

The [column kernel type proposal](column_kernel_type_proposal.md) treats a
selected frame column as `TSC<T>`. Current values expose aligned positions
across columns. Consumers use row IDs to retain state across cycles.
Compaction may change positions while preserving IDs and column alignment;
physical slot movement alone is not a row change.

## Arrow evidence and limits

The probe exports fixed-width `sparse_bunch<std::int64_t>` values as Arrow
chunks sharing BG value buffers. It splits at deleted-row gaps and BG block
boundaries. It copies `index<>` validity into Arrow bitmaps; an all-null span
without constructed BG values needs a synthetic values buffer. Arrow arrays
have a validity bitmap for null cells, not a separate live-row index. The
probe uses PyArrow buffer construction and has not tested the Arrow C Data
Interface. The probe's shared blocks can include unconstructed null slots;
an export sent outside the process must initialize or sanitize those bytes.

`Column<T>` and `Frame<SCHEMA>` can have Arrow-backed scalar runtime types that
retain Arrow buffers. An Arrow-backed scalar can expose a compatible Arrow
array or table without copying cell buffers. A BG-backed scalar can also share
compatible buffers on export, as the probe demonstrates for fixed-width
values. Building chunks, converting validity, skipping dead rows, and packing
strings or symbols may still allocate or copy. Zero-copy Arrow export is thus a
capability of a compatible runtime representation, not a guarantee of the
scalar type. An Arrow object kept after a
node invocation must own or retain its buffers; an invocation-scoped borrowed
view cannot be exported with a longer lifetime by copying only its descriptor.
Provide two explicit exports: an invocation-scoped borrowed Arrow view and a
gather-copy export with one fresh dense owning chunk per column. The latter
initializes null value bytes and survives source mutation or destruction.
Borrowing preserves source fragmentation; it does not silently gather.
An Arrow-backed mutable `TSF` would need replacement chunks, an overlay, or
copy-on-write for edits to buffers shared with immutable scalar values.

The [benchmark](benchmark_results.md) uses 100,000 physical positions and
fixed-width values. It compares sparse chunk export, a dense gather, and
Arrow sum/filter on already-built arrays. The allocation variants include a
new descriptor array for sparse export and a new NumPy values array for dense
export. Sparse export includes Python per-chunk wrapper construction; BG row
construction, mutation, a native Arrow adapter, Polars, and network transfer
are outside the measurement.

Chunk count matters more than deleted-row percentage in this probe. At 0.1%
scattered deletions, 115 chunks made allocation-inclusive export plus sum
about 1.8 times the dense path. At 1%, about 1,000 chunks made it about seven
times the dense path. Clustered deletion remained near the BG block count.
There is no validated 0.5% cutoff. An adaptive choice between shared chunks,
dense gather, and eventual compaction is plausible; its rule needs native
measurements and operator workloads. The probe does not compare Polars with
Arrow compute or establish a general engine speed ranking.

## Schema and non-numeric columns

One pivot proposal represents pivoted columns as dictionaries of whole
columns. A column path could combine fixed names with one or more pivot-key
parts. This might support runtime column sets without treating each pivot
result as a new static schema. The exact schema, type resolution, and operator
interfaces remain open.

BG strings use `std::pmr::string`, which is not Arrow's offsets-and-bytes
layout. Exporting ordinary Arrow strings requires packing offsets and bytes.
Interned symbols may suit Arrow dictionary encoding. PyArrow 24 did not
provide every tested compute kernel for `string_view` or dictionary strings.
Whether BG strings and symbols guarantee UTF-8 is unresolved. No string or
symbol representation has been selected.

## Integration and external use

The [implementation plan](frame_implementation_plan.md) starts with the remaining
utility run/block access, then numeric column storage and its BG type
integration, fixed-schema frames, selected views, and expressions.
BG's current runtime resolver enumerates
built-in time series forms, so integration must identify the smallest missing
type/provider hooks. The plan remains a proposal for implementation.

The table and delta data types should be useful from Python and potentially
for streaming to a browser. Arrow Flight was mentioned as a possible
transport, but no transport or wire delta representation has been chosen.
The data type design does not depend on choosing Flight.
