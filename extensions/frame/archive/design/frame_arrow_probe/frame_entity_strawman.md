# Frame entities, runtime types, and node APIs

Status: strawman for discussion. Entity names and API spellings below are
proposed names, not approved terminology or compilable C++. This makes the
[I/O proposal](frame_io_api_proposal.md) concrete. It assumes BG's existing
evaluation, invalidation, publication, and reference machinery.
The [working note](frame_design_note.md) records the current previous-cell and
expression contracts. The [implementation plan](frame_implementation_plan.md)
sets out a proposed first subset of these entities.

## 1. Scalar values

```text
Column<T> {
    ordered nullable cells of T
}

Frame<SCHEMA> {
    SCHEMA& schema                       // ordered names and element types
    size row_count
    Column<?> columns[schema.size]       // every column has the same row order
}
```

These describe values, not mandatory C++ objects. A fixed schema resolves a
name to a column ordinal; it needs no name dictionary in each frame instance.
The schema can retain one shared name lookup. The explicit row count also
permits a frame with rows and zero columns.

The existing two-word `Scalar<T>` is the public borrowed handle. Its runtime
type reads the backing data. `ScalarValue<T>` owns a stable value. A TSF's
`value_view()` projects its existing backing data; it does not construct a
second table. Internal backing may use row IDs; scalar operations expose
positions, cells, and nulls.

## 2. Row and cell storage

```text
RowDomain {
    identity
    row_id_allocator                     // only the origin allocates/retires IDs
}

Rows {
    retained RowDomain domain
    ordered membership of RowId
}

NativeLayout {
    occupancy_index live_slots
    ID-to-slot map                       // dense array in the initial native layout
    array<RowId> slot_to_id
}

NativeColumn<T> {
    shared NativeLayout& layout
    sparse_bunch<T> values
    index<> valid_cells                  // indexed by physical slot
}
```

`Rows` supplies `size`, `contains(id)`, `id_at(position)`, `position_of(id)`,
and ID batches. These operations do not prescribe one general ordered-index
utility. Two initial representations suffice:

```text
NativeRows {
    domain
    shared NativeLayout layout
    order = live physical slots in increasing order
}

SelectedRows {
    retained source Rows
    selected RowIds                      // a subset, iterated in source order
}
```

Native frame columns share one layout. The owning writer appends physical
rows; stable compaction reclaims holes while preserving logical order. It can
reuse logical IDs under the retirement rule defined in the
[working note](frame_design_note.md#row-changes-and-identity).
Later layouts can
use a separate order mapping if measurements justify it.

A computed column shares its input `Rows` but owns its own cell layout.
Source compaction therefore cannot move its cells implicitly. Its layout
resolves inherited IDs to its own slots. A small selection need not allocate
a dense array covering the source's entire ID range; the mapping representation
can be sparse when needed.

Within a `NativeColumn`, row liveness comes from the layout and cell nullness
comes from `valid_cells`. A live null cell may have no constructed `T`.
Existing bunch/index utilities remain the storage building blocks, subject to
the [proposed block and run access](utility_api_proposal.md).

## 3. Incremental state

```text
MembershipChanges {
    IdSet added
    IdSet removed
}

TSF<SCHEMA> backing {
    Rows rows
    column sources[schema.size]
    MembershipChanges membership
    IdSet modified[schema.size]
    BG update state for the frame and each column child
}

TSC<T> backing {
    shared Rows rows
    column source
    MembershipChanges owned_or_shared_membership
    IdSet modified
    BG update state
}
```

`IdSet` denotes a read-only set interface with batch iteration. Its storage
may be a sparse list, bitmap, ranges, or all-current-rows marker. These are
representation choices. There is no per-cell timestamp array.

A TSF column child views the parent's rows, column source, membership changes,
column modifications, and child update time. It does not duplicate this state.
A following TSC shares membership changes where their publication semantics
match, and owns its computed-cell modifications.

The public changed-live query combines additions and modifications. Internal
membership changes remain separate so the writer can initialize new rows and
share row additions/removals across columns. The frame-wide changed set is
the union of additions and modified columns, enumerating each ID once.

Two TSF backing representations are proposed:

```text
OwnedFrame {
    NativeRows rows
    NativeColumn<?> columns[schema.size]
    changes
}

ViewFrame {
    shared-or-selected Rows rows
    retained source bindings
    source-column mapping[schema.size]
    changes
}
```

Each source-column mapping retains its graph source for change information
and a `ColumnSource` that identifies the physical cell owner and column.
The column source reads cells by row ID and presents them in the output's
row order. A projection
shares rows and selects columns. A filter selects rows. Attaching a computed
TSC shares the input rows and adds another column source. Source bindings are
runtime-owned graph handles; they never retain invocation-scoped `In` views.

The default delta supplies current values and removed IDs. A consumer requests
previous cells directly from the physical owner through `ColumnSource`.
Owners capture only demanded columns; selected views supply previous
membership. The [subscription contract](frame_design_note.md#previous-cells-by-owner-subscription)
defines the current direction. Its concrete API and journal remain open.

## 4. Runtime types and kernels

The mapping has three parts:

```text
BG operator overloads:  semantic input types -> semantic result type
Runtime types:         actual backing data -> logical access and batches
Kernel implementation: typed batches -> result batches or reduction
```

A kernel does not require a new BG runtime type for every operation. Runtime
types describe data. Operation handlers implement arithmetic, predicates,
reductions, and other kernels.

### Required runtime interfaces

| Proposed runtime interface | Base | Required access |
| --- | --- | --- |
| `ColumnRuntimeType` | `ScalarRuntimeType` | Length, null-aware cell reads, batch cursor, ownership/copy and supported equality |
| `FrameRuntimeType` | `ScalarRuntimeType` | Row count, schema-indexed column access, ownership/copy and supported equality |
| `TSCRuntimeType` | `TimeSeriesRuntimeType` | Rows, current Column view, row delta, ID-based reads and writer operations |
| `TSFRuntimeType` | `TimeSeriesRuntimeType` | Rows, current Frame view, row delta, TSC children and writer operations |

The matching semantic declarations are `Column<T>`, `Frame<SCHEMA>`, `TSC<T>`,
and `TSF<SCHEMA>`. Runtime types carry the resolved element types or schema.

### Backing representations to support

| Value | Initial representations |
| --- | --- |
| Column | Dense owned buffers; bunch-backed values; retained Arrow arrays/chunked arrays; selected source access |
| Frame | Ordered column sources with a row count; those sources may be native, Arrow-backed, or selected |
| TSC | Owning column; computed column following input rows; child of a TSF |
| TSF | OwnedFrame; ViewFrame |

These are layout requirements, not a requirement for a distinct C++ class for
every table cell in the matrix. A representation can use shared implementation
or tagged data where clear. One composed Frame runtime can expose Arrow column
sources without requiring an extra Arrow-table-specific frame runtime.

An owning scalar copied from live mutable backing acquires stable storage.
An Arrow-backed owner can retain compatible immutable buffers. This follows
BG's existing `Scalar`/`ScalarValue` ownership distinction.

### Batch and kernel boundary

```text
ColumnBatch<T> {
    size logical_length
    encoding = flat | bits | constant | dictionary
    typed buffers appropriate to encoding
    validity                            // all-valid or an explicit null mask
    optional selection                  // logical index -> encoded value slot
}

KernelPlan {
    resolved input and result element types
    operation and its null/options contract
    implementation for typed batch access
}
```

`ColumnBatch` is an invocation-scoped execution descriptor, not a graph value.
In a flat batch, validity is tested at the selected physical slot. Dictionary
and Boolean batches define their corresponding validity/read rules through
their encoding adapter. Kernels must not interpret dictionary codes as values
from another dictionary. Encodings that a kernel cannot consume use an explicit
decode/gather fallback.

A multi-column reader yields aligned batches and optional ID batches. It splits
at compatible input boundaries or gathers into scratch buffers when required;
columns do not need identical chunk boundaries. ID lookup and runtime dispatch
occur per batch where possible. Resolved implementations can be cached while
the relevant runtime types and operation options remain unchanged.

BG operator overloads resolve operations such as
`add(Column<double>, Column<double>) -> Column<double>`. A typed kernel then
consumes the batch descriptors. A TSC node determines affected IDs and invokes
that same kernel on those rows. Arrow compute is an optional implementation
for compatible batches. Existing `ScalarDispatch<Handler>` can register optional
conversion/backend handlers; its single-runtime key alone does not resolve a
binary operation across arbitrary representations.

### Expressions at wiring and evaluation

Arithmetic overloads can return ordinary `wp<TSC<T>>` points backed by
expression records. Existing materialisation creates a computed column when
a concrete TSC is required. Frame operators can consume the deferred
representation while wiring their nodes.

The proposed runtime scalar is `Expression<SCHEMA, RESULT>`: an immutable plan
with bound scalar parameters. An ordinary node constructs this value from
changing scalar time series and publishes `TS<Expression<SCHEMA, RESULT>>`.
This separates expression construction from frame-node implementation.
The [expression contract](frame_design_note.md#expressions-and-wiring)
defines binding, materialisation, and affected-row rules.

## 5. High-level public reads

Signatures below show behavior. `cell` denotes null or a typed value; it is
not a committed nullable scalar declaration.

```text
Scalar<Column<T>>
    size()
    read(position) -> cell<T>
    batches(positions = all) -> ColumnBatch<T> batches

Scalar<Frame<SCHEMA>>
    size()
    column<field>() -> Scalar<Column<T>>

In<TSC<T>>
    value_view() -> Scalar<Column<T>>
    rows() -> borrowed Rows
    changes() -> { changed_live_ids, removed_ids }
    read(id) -> cell<T>
    batches(ids) -> aligned { ids, ColumnBatch<T> } batches

In<TSF<SCHEMA>>
    value_view() -> Scalar<Frame<SCHEMA>>
    rows() -> borrowed Rows
    changes() -> { changed_live_ids, removed_ids }
    column<field>() -> In<TSC<T>>
    batches<fields...>(ids) -> aligned { ids, column batches... }
```

Public typed field access should use BG's schema conventions; `column<field>`
is shorthand here. Runtime name lookup can return an erased column view.
Scalar positions and time-series IDs are separate arguments. An ID batch
request selects live rows; a missing row is not a null cell.

Batch views borrow storage for their callback/cursor step. Indexed reads return
small decoded primitives by value and string-like values as invocation-scoped
views. Generic cell visitation can use callback-scoped scalar views. This
supports Arrow Boolean bits without inventing an addressable stored `bool`.

## 6. High-level public writes

The node definition selects its producer capability through the output
declaration. `Out<TSF<SCHEMA>>` selects the default writer;
`Out<TSF<SCHEMA>, view>` selects the view writer. `TSC` follows the same rule.
The specification selects the runtime representation and available writer
operations without changing the semantic output type. Runtime selection uses
the resolved output type and this specification. Source runtime information
belongs to the view's source bindings.

```text
Owning TSF writer
    add(complete_row) -> RowId
    append(frame_value) -> inserted ID batches
    set<field>(id, cell)
    set_batch<field>(ids, column_values)
    erase(ids)
    clear()

Owning TSC writer
    add(cell) -> RowId
    append(column_values) -> inserted ID batches
    set(id, cell)
    set_batch(ids, column_values)
    erase(ids)
    clear()

Following TSC writer
    follow_rows(input)
    set(id, cell)
    set_batch(ids, column_values)

View TSF writer
    follow_rows(input)                   // share complete membership and order
    bind_rows(input)
    bind_column<output_field>(source_column)
    select_rows(ids)                      // initial/rebuilt selection
    include(ids)
    exclude(ids)
    forward_changes(source, ids)          // mapped columns and selected rows
```

`follow_rows` selects shared complete membership; `bind_rows` supplies the
domain and order for a producer-maintained selection. A producer chooses one
row strategy. Binding records stable graph sources. Bindings are established
during producer initialization and replaced explicitly on rebinding. A view's bindings must
describe every output column. All bound columns must cover its selected IDs.

`follow_rows` reconciles membership, releases owned result cells for removed
rows where present, and shares current order. A computed producer must fill
every newly added result cell before
publication. `include` records a complete newly visible row; `forward_changes`
records only mapped column modifications for continuing selected rows. The
writer normalizes repeated operations into one completed delta.

Batch writes accept compatible column views or typed batches and consume their
values during the invocation; they do not retain scratch kernel buffers.
Kernel implementations may write through an output
batch sink to avoid an intermediate result allocation. Full-row insertion
requires all fields. Null writes are explicit; omitted patch columns are left
unchanged. A parent-column writer provides only cell writes.

## 7. Example nodes

These are short node-body sketches. An annotation such as `[view]` selects
the writer capability; it is not C++ syntax. Initial evaluation and rebuilding
use all current rows. Later evaluations use current changes. BG handles output
publication after the node finishes.

### Owning source with an optional business key

```text
Quotes = { symbol: symbol, bid: double, ask: double }

node quote_source(events) -> TSF<Quotes> [owning]:
    state ids_by_symbol

    for event in events:
        if event is insert:
            ids_by_symbol[event.symbol] = out.add(event.complete_row)
        if event is update:
            id = ids_by_symbol[event.symbol]
            for supplied field in event.patch:
                out.set<field>(id, event[field])
        if event is remove:
            out.erase(ids_by_symbol.remove(event.symbol))
```

The ingestion node defines duplicate-key behavior. Its key index belongs to
the node; unkeyed sources simply allocate rows without it. Batch ingestion uses
`append` and `set_batch` instead of the event loop.

### Computed TSC, retaining input IDs

```text
node midpoint(q: TSF<Quotes>) -> TSC<double> [following]:
    out.follow_rows(q)
    ids = q.rows() if rebuild else union(q.bid.changed, q.ask.changed)

    for batch in q.batches<bid, ask>(ids):
        result = kernel((bid + ask) * 0.5, batch)
        out.set_batch(batch.ids, result)
```

The expression can execute in one loop. It propagates null cells and owns only
the result column. Row removals are handled by `follow_rows`. Newly added rows
appear in both input column changes and receive a result, including null.
This illustrates a node body; deferred expressions can generate its plan and
can also execute through several batch kernels with temporary scratch.

### Filter sharing all source cells

```text
node positive_bid(q: TSF<Quotes>) -> TSF<Quotes> [view]:
    on binding:
        out.bind_rows(q)
        bind every output column to the corresponding q column

    if rebuild:
        out.select_rows(ids where q.bid > 0)
    else:
        out.exclude(q.removed)
        for batch in q.bid.batches(q.bid.changed):
            mask = kernel(bid > 0, batch)
            out.include(batch.ids where mask is true)
            out.exclude(batch.ids where mask is false or null)

    out.forward_changes(q, selected IDs)
```

Predicate work depends only on `bid` changes. Changes to `ask` still propagate
for selected rows. Newly included rows expose all columns. The output owns its
selection and deltas; its cells remain in the source. Change forwarding visits
the source's changed IDs intersected with the selection, not all selected rows.

### Filter with a changing scalar threshold

```text
wire filter(q, q.size > min_size):
    plan = comparison(column(size), parameter(min_size))
    predicate = expression_node(min_size, plan)  // TS<Expression<Quotes, bool>>
    return filter_node(q, predicate)

node expression_node(min_size: TS<int64>, plan) -> TS<Expression<Quotes, bool>>:
    out.set(plan with an owned current min_size value)

node filter_node(q: TSF<Quotes>, predicate: TS<Expression<Quotes, bool>>)
    -> TSF<Quotes> [view]:
    bind source rows and columns as in positive_bid
    out.exclude(q.removed)
    ids = q.rows() if rebuild or predicate.modified()
          else union(added rows, changed rows of predicate columns)
    evaluate predicate on ids into output membership
    out.forward_changes(q, selected IDs)
```

For this example extend `Quotes` with `size: int64`. The expression node runs
on threshold changes; column-only updates reuse its current expression value.
The filter signature stays the same for any number of scalar parameters.
This separation simplifies node code. Constant expressions use `const_`.

### Attach a computed column without copying existing columns

```text
node add_mid(q: TSF<Quotes>, mid: TSC<double>) -> TSF<QuotesWithMid> [view]:
    require same row IDs, membership, and order

    on binding:
        bind symbol, bid, ask to q's columns
        out.bind_column<mid>(mid)

    out.follow_rows(q)
    out.forward_changes(q, all current rows)
    out.forward_changes(mid, all current rows)
```

This view shares every cell, including the computed column's storage. A plain
projection uses the same representation with fewer column bindings. Membership
removals use the shared membership delta and do not require reading removed
cells. Forwarding is proportional to relevant changes; `all current rows` here
specifies the selection, not a request to scan every row.

## Node rules and decisions still open

- Nodes must initialize/rebuild when retained state cannot consume the delta.
- Nodes borrowing cells must propagate relevant source changes.
- Nodes invoking positional kernels must establish alignment.

The remaining design choices are generic nullable-cell access, the
previous-value subscription API, and the expression wiring adapter.
Dynamic pivot column sets and sorted-result publication remain later work.
