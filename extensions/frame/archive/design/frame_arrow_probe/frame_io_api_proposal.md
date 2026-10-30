# Frame and column input/output API proposal

Status: discussion proposal, not an approved BG design or implementation.
Names and signatures below describe the intended contract; they are not
compilable C++. This proposal builds on the [frame working note](frame_design_note.md)
and the [column kernel proposal](column_kernel_type_proposal.md). Existing BG
contracts are in the [scalar API](../../docs/scalar-api.md),
[node API](../../docs/node-api.md), and [evaluation design](../../docs/evaluation.md).
The [framework and comparative review](frame_api_review.md) explains the
revisions below, including findings from Arrow, Polars, kdb+, Perspective,
Deephaven, DuckDB, and Feldera. Review recommendations remain unapproved.
The [entity and API strawman](frame_entity_strawman.md) gives concrete backing
sketches, runtime interfaces, and node examples for these contracts.
The [working note](frame_design_note.md) supplies the latest expression and
previous-cell direction; the [implementation plan](frame_implementation_plan.md)
proposes the first implementation slices.

## Model established by the discussion

| Scalar value | Time series with row changes |
| --- | --- |
| `Column<T>`: one ordered sequence of nullable cells | `TSC<T>`: current column and row changes |
| `Frame<SCHEMA>`: aligned columns and logical row order | `TSF<SCHEMA>`: current frame and row changes |

The input/output sketches below use a fixed schema. Dictionaries of whole
columns for pivot results remain a separate schema question.

`TS<Column<T>>` and `TS<Frame<SCHEMA>>` publish complete scalar values. They
provide no row-change queries. Their published values remain stable until the
next update. `TSC<T>` and `TSF<SCHEMA>` mutate through their owning node's
output and provide current values through borrowed scalar views. Constructing
those views does not copy cells. A view supplied to a node is valid during that
invocation and cannot be retained.

`TSF` supports row addition, removal, and modification. It has no in-place
reorder operation. Filtering and sorting are nodes that produce their own
outputs. A frame need not have a user key. Frame-library nodes need an opaque
row ID that remains stable for a live row across cycles and physical moves.
An ID has a defined domain; equal numeric IDs from unrelated domains do not
identify the same row. Physical slots and logical positions can change
independently of IDs. The review recommends preserving source IDs in filters,
column projections, and row-preserving expressions. Group and general join
outputs allocate their own identities.

A selected frame may be represented by its own `TSF` state containing a source
reference and a row selection. It owns its membership, change record, and
update times while borrowing selected cell values from the source. The graph
type seen by consumers remains `TSF<SCHEMA>`. An Arrow-backed scalar is another
runtime representation of `Column` or `Frame`. Arrow export shares buffers
only when the source layout permits it.

## Scalar reads and publication

`Scalar<Column<T>>` and `Scalar<Frame<SCHEMA>>` are read-only views. A scalar
frame exposes row count and typed column selection. A scalar column exposes
length, null-aware indexed access, and a bulk-read interface as developed in
the [column kernel proposal](column_kernel_type_proposal.md). Column names
belong to the frame schema. Neither scalar exposes row IDs or change records.

Illustrative read operations:

```cpp
auto frame = input.value_view();                 // Scalar<Frame<Quote>>
auto price = frame.column<"price">();           // Scalar<Column<double>>
auto cell = price.cell(position);                // optional borrowed Scalar<double>
```

The exact C++ spelling of typed field selection and nullable cells remains
open. Typed field access should follow BG's shared schema model. A bit-packed
Boolean or decoded dictionary cell may need callback-scoped access or a typed
read; the optional borrowed scalar above is not yet a complete layout-neutral
cell API. A null cell, a removed row, and an invalid time series are distinct.
Every column in one frame has the same logical row count and order.

`ScalarValue<Column<T>>` and `ScalarValue<Frame<SCHEMA>>` own or retain their
buffers. A node publishes one through `Out<TS<Column<T>>>::set_value()` or
`Out<TS<Frame<SCHEMA>>>::set_value()`, following the existing scalar API.
There is no `Out<Column<T>>` or `Out<Frame<SCHEMA>>`: scalars are values,
not time series outputs. A value made from a mutable `TSC` or `TSF` view must
acquire stable backing storage before publication. Retaining an Arrow table's
immutable buffers can avoid copying them; sharing mutable source buffers
requires freezing or copy-on-write.

## Time series inputs

Both new time series provide BG's `valid()`, `modified()`, and
`last_updated()`. Their additional input contract is:

| Input | Current value | Row information |
| --- | --- | --- |
| `In<TSC<T>>` | `Scalar<Column<T>> value_view()` | Live, changed, and removed row IDs; ID lookup |
| `In<TSF<SCHEMA>>` | `Scalar<Frame<SCHEMA>> value_view()` | Row IDs, ID lookup, and typed `TSC` columns |

The following callback shape is a proposed way to read changes without
creating a time series per row:

```cpp
frame.for_each_row([](row_id_t id, std::size_t position) { /* current row */ });
frame.for_each_changed([](row_id_t id) { /* read current row by ID */ });
frame.for_each_removed([](row_id_t id) { /* no current row */ });
auto position = frame.position_of(row_id);       // optional current position

auto price = frame.column<"price">();          // In<TSC<double>>
price.for_each_changed([](row_id_t id) { /* read current cell by ID */ });
price.for_each_removed([](row_id_t id) {});
```

`position` indexes the current scalar value. Inputs also provide direct
ID-based cell and batch reads. `position_of(id)` can require a rank lookup in
a sparse selection; incremental nodes need not pay that cost for every cell.
The callbacks are conveniences over a batch interface for rows, changes,
and selected columns. The exact C++ operations remain to be proposed.

Inputs need both a row-domain check and a way to establish equal current
membership and order before positional operations. Two filters can share a
domain while selecting different rows. A shared description of membership
and order provides a cheap alignment proof; otherwise align explicitly.
The callbacks and their views have invocation scope. On the first publication,
every live row appears as changed, including when the value was constructed
from an Arrow table. Publishing an initially empty frame has no row changes
but still updates the time series. A later `clear()` reports previously live
rows as removed while leaving a valid empty frame.

**Revised validity recommendation:** ordinary and deduplicated `TSC` and `TSF`
support invalidation. They start invalid, first publication validates them,
and `clear()` publishes a valid empty value. Invalidating a frame invalidates
its column views; revalidation supplies a complete current value. A borrowed
selected output cannot preserve an old value solely by preserving its mask.
The [review](frame_api_review.md#1-borrowed-outputs-must-obey-publication)
explains the required producer constraints. This replaces the tentative
TSS/TSD validity precedent and still requires agreement.

A node that first observes an already valid source, including after a
reference changes target, initializes from `for_each_row()` and
`value_view()`. It uses changed and removed IDs on later updates. Change
queries describe a completed current-cycle update, not a history since a
particular node last observed the source.

The input API must make that distinction observable: provide an opaque
publication token, the base token for the delta, and explicit reference
resampling information. A consumer retains its last consumed token. It
rebuilds after a gap or resampling and applies a delta only to its matching
base while that delta is available in the current cycle. Once change records
are cleared, reading a newer publication requires a rebuild even if its base
token matches. Tokens distinguish logical-series incarnations, including selections
in the same row domain. This requires integration with BG reference sampling;
it is not an existing BG API.

`changed` combines added and modified live rows. The discussion found no need
for a mandatory distinction between them. A node can compare IDs with its
own retained state. A frame-wide changed row appears when any cell in that
row changes. A `TSC` child reports a changed row only when its cell changes or
the row enters its parent frame. Row removal appears on the frame and every
column child. One last-updated time per column and sparse current-cycle ID
sets suffice; no row-by-column timestamp matrix is needed.
Additions and removals can share one record across columns. Batch changes
need range/all-row forms so dense updates do not require a per-cell record.
Exact per-column changes remain the contract; a frame-wide row set and column
set alone can describe a larger rectangle of cells than actually changed.

The proposed delta describes the completed update: a changed ID identifies
a live row, and a removed ID identifies a row absent from the current value.
The two sets are disjoint. An add followed by removal in one invocation may
leave an empty delta while still publishing an accepted mutation, as BG
already permits for collection time series. ID reuse must not make an old
removal and a new row ambiguous in the same cycle.
Removed IDs must belong to the preceding published membership. A transient
addition never seen by consumers supplies no removal. Repeated operations
are interpreted relative to that preceding publication, with one entry per ID.

Removal callbacks provide row IDs. Consumers can opt into previous-cell
access for the columns they need. `ColumnSource` identifies each physical
owner, so the subscription registers there directly. The owner shares
cycle-scoped capture across consumers; a selected view supplies previous
membership. Undemanded columns capture no old cells. The
[previous-cell contract](frame_design_note.md#previous-cells-by-owner-subscription)
defines the required lifetime and meaning. Subscription spelling and capture
storage remain to be specified and measured.

Selecting a fixed-schema column from `In<TSF<SCHEMA>>` returns an
`In<TSC<T>>` with the frame's row IDs and its own change time. The matching
graph-wiring operation selects a `TSC<T>` edge. Selecting a column from
`TS<Frame<SCHEMA>>` instead produces `TS<Column<T>>` through a node because
the source has no row changes. Row-wise operators must establish alignment;
equal lengths or equal numeric row IDs from unrelated frames do not suffice.

## Materialized time series outputs

The proposed public `Out<TSF<SCHEMA>>` writer changes its own rows. Its
smallest useful operations are:

```cpp
auto id = output.add_row(complete_row);          // allocate an output row ID
output.set_cell<"price">(id, new_cell);          // existing row; null is a cell value
output.erase_row(id);
output.clear();                                  // remove all rows; publish valid empty
```

`complete_row` means values or nulls for all schema fields. Its C++ type is
open: it may reuse `Bundle<SCHEMA>` where that expresses null cells correctly.
Partial update omission must be distinct from a null cell. A required frame
column is not an optional Bundle field or optional node input.
`set_cell` changes one column child's change record. A whole-row update can
be a convenience operation that calls the same per-column mechanism. Row
addition and removal update all column children. Repeated writes in one node
invocation produce one coherent published frame after the function completes.
The existing [evaluation failure rule](../../docs/evaluation.md#exceptions)
does not promise rollback after an exception.

A node using this writer selects a materialized output runtime. A selected
view runtime needs a producer writer for membership and source binding; it
cannot be passed to this materialized row writer. This selection belongs to
the producer's compiled runtime configuration, not the graph type.
The current runtime resolver can inherit an exact semantic match from input
runtime types. Output construction must explicitly select and validate the
producer's layout so a materialized writer cannot inherit selected storage.

The materialized `Out<TSC<T>>` writer can own a row domain and provide the
same `add_row(cell)`, `set_cell(id, cell)`, `erase_row(id)`, and `clear()`
semantics for a single column. A `TSC` projected from a `TSF` does not own
membership. Its parent output controls row addition and removal; a column
writer obtained from the parent may set cells only for existing rows. This
keeps all frame columns aligned. A row-preserving expression writer follows
its input's membership and order, preserves its IDs, and writes computed cells
for those IDs. It cannot use the owning writer's fresh-ID allocation for this
purpose. This producer capability and its C++ spelling need definition.

The review recommends insertion order for owning writers, with survivor order
preserved after deletion. Filters preserve source order. Compaction preserves
logical order, which can require stable moves or a separate order mapping.
An incremental sorted-result order contract remains separate work; no in-place
reorder operation is proposed.

Define complete-row batch insertion, column updates by ID, and batch reads in
the first API. Batch and single-row writes have the same publication semantics.
Row ID allocation, reuse, and concrete batch descriptors remain open.

## Selected-view output

A filter node can publish a `TSF<SCHEMA>` through a selected-view runtime type.
The expression API supplies `TS<Expression<SCHEMA, bool>>`; a separate ordinary
node constructs it from changing scalar inputs. A filter can also consume an
aligned materialised `TSC<bool>`. The
[expression contract](frame_design_note.md#expressions-and-wiring) defines the
public syntax and which rows require predicate evaluation. Null predicate
cells are excluded.
Its producer changes membership and records changed IDs; it does not write
cells into the source. It combines predicate effects with source membership
and payload changes. A previously selected row that still qualifies is changed
when its source row changes. A row entering or leaving the selection is added
or removed in the output. Excluded source rows cause no output update.

The output must retain the source storage through a graph dependency or
owned handle, not retain the invocation-scoped `In<TSF>` view. Selection state
should use row IDs, so physical source compaction cannot alter membership.
Every mutable dependency supplying cells or membership must be active and
participate in evaluation order. Source invalidation/removal, predicate
invalidation, and reference resampling must be handled before downstream reads.
Retaining source storage alone does not preserve its previous value. A producer
that preserves or samples an output needs stable backing; a borrowed producer
invalidates and rebuilds instead. `final`, `const`, and deduplicated output
guarantees also require proof or appropriate owned storage.
The selected output has its own last-updated time and column-child change
records. It cannot be the source `TSF` merely reticked with a different mask.
Its producer writer needs operations equivalent to `include(id)`,
`exclude(id)`, and `changed(id, affected_columns)`. It binds to one source
and never edits source cells. These operations update the selected output's
membership and change record.

The first proposal keeps this producer writer inside the frame library and
does not add `Out<TSF<SCHEMA>, masked>` to the public node API. The current
`Out` template accepts one time series declaration, and a mask is a runtime
representation choice, not part of `TSF`'s graph type. If several public
nodes need to author selected views, add an output-only writer API that binds
to this runtime type. Consumers continue to use `In<TSF<SCHEMA>>`.

## Arrow boundary

An Arrow-backed `ScalarValue<Frame<SCHEMA>>` can retain compatible Arrow
buffers. A borrowed Arrow view of `Scalar<Frame<SCHEMA>>` has the same short
lifetime as its BG source. An Arrow table that escapes the invocation must
retain immutable buffers. Export from BG sparse storage may share fixed-width
value buffers while allocating chunk descriptors or validity bitmaps. A
selected view can share contiguous selected runs; a scattered mask may need
many chunks or a gather. String and symbol conversion may also copy. Neither
`value_view()` nor `TSF` promises a universally zero-copy Arrow table.

## Decisions needed before implementation

1. Confirm invalidation and activity rules for borrowed outputs, including
   source removal and all BG time series guarantees.
2. Define publication continuity and reference resampling in the input API.
3. Define row identity reuse, shared membership/order, and computed TSC writers.
4. Specify direct owner subscriptions and previous-cell access; compare capture
   for demanded columns with consumer caches to choose storage and batch paths.
5. Specify nullable cells, complete rows, and omitted patch columns, together
   with batch input/output descriptors and schema matching.
6. Confirm insertion/source order semantics and producer-selected runtime
   capabilities. Define a public writer interface if third-party producers
   need it. Consumers retain one TSF/TSC input contract.
7. Specify expression scalar storage and its wiring adapter using existing
   materialisation. Frame nodes receive constructed expressions through `TS`.
