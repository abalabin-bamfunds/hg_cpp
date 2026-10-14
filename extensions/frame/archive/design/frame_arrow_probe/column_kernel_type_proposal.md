# Column kernel type proposal

Status: discussion proposal. `Column<T>`, `TSC<T>`, `TSF<SCHEMA>`, and
`Nullable<T>` are provisional names, not approved project terminology or an
implementation commitment. This proposal uses the
[frame working note](frame_design_note.md) and BG's existing
[type system](../../docs/type-system.md) and [scalar API](../../docs/scalar-api.md).
The [framework review](frame_api_review.md) supplies the latest recommendations
on alignment, continuity, borrowed-output validity, and batch access.

## Recommendation

Use `Column<T>` and `Frame<SCHEMA>` for scalar values, and `TSC<T>` and
`TSF<SCHEMA>` for time series with row changes. A selected column
`frame["price"]` should denote `TSC<Price>` for a `Price` field. A scalar
column represents a finite, ordered sequence of cells of type `T`. Each cell can
be null. `T` identifies the value type when a cell is present; null is separate
from any value of `T`, including NaN. The sequence has a logical length and
indexed cell access. It has no name, row IDs, update record, or time axis.
Nullability need not allocate a bitmap when every cell is present; the runtime
representation can report an all-valid batch.

`Scalar<Column<T>>` is a borrowed batch view read during one node invocation;
`ScalarValue<Column<T>>` owns or retains a stable batch. `TS<Column<T>>` is a time
series of complete column values and has no row-change queries. `TSC<T>` has
current column access, row identity, and row-change queries. `TSF<SCHEMA>`
has a current scalar `Frame<SCHEMA>` value, owns its membership and change
record, and projects its columns as `TSC<T>` children. Reading the scalar value
from either time series must borrow its cells without copying them.

## Time-series column and frame projection

`In<TSC<T>>` should expose a current `Column<T>` view and the row IDs added,
modified, or removed in the current cycle. A modification exposes the current
cell. Whether removal exposes a prior cell remains open; the
[input/output API proposal](frame_io_api_proposal.md) recommends an ID only.
It also has the normal BG validity and last-updated queries. It does not
create one BG time series per cell. The proposed public delta supplies current
modified values; optional cycle-scoped old-value storage remains a review
question rather than a requirement of this type.

For a fixed-schema `TSF<SCHEMA>`, `frame["price"]` selects a `TSC<Price>` child.
The frame runtime owns or retains its row domain, order, and column buffers.
The child refers to that state, then constructs a borrowed column view when a
node reads it. The time-series state remains current between cycles; only the
borrowed view has invocation scope. A standalone `TSC<T>` may own its domain
or follow another series' domain. The I/O proposal recommends that a
row-preserving expression writer inherit its input IDs, membership, and order
while owning computed cells; its concrete C++ contract remains open.

Frame row additions and removals affect every child and can share one change
record. A cell write records its row ID for the affected child during the
current cycle. A whole-row update may mark every child. Each child has one
last-updated time; no row-by-column timestamp matrix is needed. The frame-wide
delta combines row additions, removals, and modifications. Physical
compaction changes neither the logical row order nor any child value and
should not publish an update.

`add(frame["bid"], frame["ask"])` can union the two `TSC` change sets and update
only those output rows. It must establish that both inputs use the same row
domain, membership, and order before positional kernel execution. Filters of
the same domain can select different rows. Equal row ID numbers from unrelated
frames do not establish alignment.
Joining or aligning different row domains is a separate operator. A result
that preserves rows can share the domain; filtering and sorting nodes define
their own result row mapping and order.

A frame runtime could expose `TSC` children through BG's collection-item
mechanism, giving each child one authoritative time-series state and change
record. For a fixed schema and fixed name, wiring can resolve the selected
`TSC<T>` type. An ordinary string-keyed C++ `operator[]` may still return a
generic wiring point when different names select different `T`, as with
heterogeneous bundle fields. Typed schema access can preserve `T`. A runtime
name in a heterogeneous schema needs a common type or a generic result.

`TS<Column<T>>` remains useful for a whole-column replacement value with no
row-change queries. An independent producer of that type must own or retain
its buffers while its value is current. `TS<Frame<SCHEMA>>` similarly gives a
whole-frame value, while live frame row changes belong to `TSF<SCHEMA>`.
Selecting a column from a scalar `Frame<SCHEMA>` yields a scalar `Column<T>`.
Selecting it from `TS<Frame<SCHEMA>>` requires a projection that publishes
`TS<Column<T>>`; it cannot infer row changes. Selecting it from `TSF<SCHEMA>`
can expose a `TSC<T>` child with the frame's row domain and change information.

`Column<T>` is a scalar because it is one batch value. `TSC<T>` is a time series
because it tracks changes to that value. `TSL<TS<T>>` would give each cell an
individual time-series identity and storage contract; this proposal keeps
cell changes in sparse per-cycle records instead.

An Arrow `Array`, Arrow `ChunkedArray`, BG bunch view, or owned kernel result
can implement the same semantic column type through different runtime types.
Chunk count, buffer alignment, dictionary encoding, and physical sparsity are
runtime representation facts. They do not change `Column<T>`'s type identity.

## Why `List<T>` is not enough

BG's `List<T>` is already an ordered, homogeneous scalar collection. Its
borrowed view is indexed, and its runtime interface supplies a stored `T` at
every position. `MutableList<T>` supports append, set, erase, and clear. A
fixed-width, dense, non-null Arrow array could be adapted to a list in
principle. A new semantic type is not justified by Arrow storage or vectorized
execution alone.

Null cells change the value contract: `Scalar<List<T>>::operator[]` returns
`Scalar<T>` and has no way to report an absent cell. BG has no general nullable
scalar type today. `List<Nullable<T>>` could express the nulls if such a scalar
type were added, but the present list runtime still requires an addressable
stored item and mutable item operations. Arrow bit-packed Boolean values and
dictionary-encoded values do not expose a stored BG `T` at each position.
Making every Arrow-backed list satisfy that interface would require decoding,
materialization, or a second fast-path interface. A read-only Arrow view also
cannot honestly implement list mutation without copy-on-write or a separate
mutable representation.

| Property | `List<T>` now | Proposed `Column<T>` |
| --- | --- | --- |
| Cell at every index | A value of `T` | Value of `T` or null |
| Primary access | Indexed scalar item | Bulk spans and validity, with indexed fallback |
| Mutation contract | Mutable list view and runtime operations | Read-only view; owner or builder creates results |
| Physical shape | Hidden behind runtime type | Hidden behind runtime type; may be chunked or selected |
| Equality and hash | Available when `T` supports them | No default equality or hash requirement |
| Row identity and changes | None | None |

The distinct type supplies a stable null-aware, read-only bulk contract.
General bulk-access machinery may also serve `List<T>` later. Column equality
can be supported when its element semantics permit it; deduplicated scalar
publication must either have that capability or be rejected during wiring.

## Kernel contract

The column runtime interface should expose logical length, cell validity, and
an efficient way to visit typed batches. A batch describes values, validity,
and its logical position range. Keep selection and encoding information
available to kernels that can consume it; an adapter may gather or decode
when required. Resolve runtime dispatch per batch. Indexed fallback must
account for layouts such as bit-packed Boolean cells, which have no stored
BG `bool` to reference. Kernel code must not assume one contiguous allocation.
A column that borrows frame storage is valid only for
the current node invocation, following BG's scalar-view lifetime rule.

At wiring time, BG operator overloads would resolve element and result types,
for example `Column<T> -> Column<U>` or `Column<T> -> Nullable<U>` for a
reduction. Runtime dispatch would choose an implementation for the actual
buffers and element type. BG's `ScalarDispatch` can register exact semantic or
runtime handlers, but a useful kernel interface also needs bulk buffer access;
generic indexed `Scalar<T>` calls alone would discard the performance benefit.

The following signatures describe behavior, not a proposed C++ API:

```text
add(Column<T>, Column<T>)       -> Column<T>       // equal lengths; align by position
sum(Column<T>)                  -> Nullable<T>     // null if no non-null inputs
filter_mask(Column<bool>)       -> Selection       // positions; null-mask rule specified by operator
sort_indices(Column<T>)         -> Permutation     // positions in chosen order
take(Column<T>, Selection)      -> Column<T>
```

Each kernel must state its null behavior. Arithmetic normally propagates nulls;
aggregates may skip them and return null when no values contribute. The result
of an all-null reduction is a *valid computation with a null result*. BG time
series invalidation means absence of a current time series value, so it cannot
silently stand for that result. A general nullable scalar result type or an
equivalent explicit result contract is needed for reductions.

Kernel outputs may be borrowed views, owned buffers, or builders reused by a
frame node. An independently published `TS<Column<T>>` must own or retain its
buffers for the time series value's lifetime. A frame projection can instead
refer to parent-owned storage. It cannot retain an invocation-scoped view.

## Frame alignment and row changes

A frame can enumerate its live rows in one logical order and give every column
the same position order. The frame retains the position-to-row-ID mapping.
Same-length kernels preserve that mapping. Filtering and sorting return a
selection or permutation. The enclosing node uses it to build its output frame
and row mapping without reordering its input. A column alone cannot identify a
row across cycles, and an equal length does not establish alignment between
columns from unrelated frames.

This allows column kernels to be useful for dense batch work while the frame
library handles incremental changes. A kernel over the whole column reads the
whole column even if one row changed; zero-copy input does not make that work
incremental. Frame nodes should use row deltas and retained node state for
small changes, and batch kernels for rebuilds or sufficiently large changes.
The threshold needs workload measurements.

## Relationship to Arrow and Polars

`Column<T>` is closest to a Polars `Series` or an Arrow `ChunkedArray` as a
typed, nullable, ordered sequence. A single Arrow `Array` is one possible
physical batch. A Polars `Series` also has a name; this proposal keeps names in
frame schema so renaming does not change column value identity. Neither Arrow
nor Polars row position supplies the stable row ID required by incremental
frame nodes.

The proposed value contract is useful outside frames for imported Arrow data,
batch processing, and nodes that publish a complete vector. It is not a
replacement for `List<T>`: small general scalar collections still benefit from
list mutation, item access, and value equality or hashing. The frame projection
puts the null-aware bulk contract on a typed graph edge, which is a concrete
reason to give it its own semantic type.

Arrow compute kernels can be one backend for compatible types and buffers.
Polars supplies useful models for deferred expressions, query planning,
parallel execution, and data layout. A deferred expression does not guarantee
a single fused loop. The probe establishes no general Polars/Arrow speed
ratio. The useful performance goal is to
make batch access and ownership explicit enough to avoid per-cell virtual
calls, unnecessary gathers, and intermediate allocations, while retaining a
delta path for small ticks.

The [expression contract](frame_design_note.md#expressions-and-wiring) places
deferred operations in BG wiring records. Changing scalar inputs construct an
expression scalar in a separate ordinary node. Kernel execution and retained
intermediate storage remain implementation choices behind that interface.

## Type-system work implied by the proposal

1. Define semantic `Column<T>` and `Frame<SCHEMA>` types and matching, plus
   their scalar runtime interfaces. Register BG-backed and Arrow-backed
   layouts. The current `TypeVisitor` lists built-in scalar forms, so
   structural visitation and diagnostics may need extension.
2. Define nullable scalar results, or another explicit representation that can
   cross node boundaries. Decide whether that same type can serve nullable
   list items; do not equate null with an invalid time series.
3. Define borrowed views, owned values, and lifetime rules. Check how
   `ScalarValue`, `Out<TS<T>>::set_value`, runtime input compatibility, and
   `State<T>` handle the layouts. A snapshot shared with a mutable `TSF` or
   `TSC` needs retained immutable buffers or copy-on-write.
4. Add a kernel dispatch boundary that receives typed batches and can adapt
   Arrow arrays, Arrow chunked arrays, and BG storage. Keep Arrow-specific
   types in the extension, not the core scalar interfaces.
5. Define `TSC<T>` with a zero-copy current `Column<T>` view, row identity,
   and sparse current-cycle row changes. Define `TSF<SCHEMA>` with a zero-copy
   current `Frame<SCHEMA>` view and frame-wide row changes. Check whether its
   runtime can expose `TSC<T>` children through BG's collection-item mechanism
   while sharing row membership and keeping one authoritative state.
6. Have column and frame operators carry row IDs alongside column positions
   and explicitly apply selections or permutations. This is independent of
   the scalar column type.

## Questions to settle before implementation

- Define one primitive and one bit-packed or dictionary-encoded type through
  the proposed batch and nullable-cell interfaces. Which bulk machinery can
  also serve existing scalar collections?
- Confirm the I/O proposal's insertion order for owning writers and source
  order for filters and row-preserving expressions. Compaction must preserve
  this observable order.
- Which column element types can the first kernel set handle directly? Fixed
  width primitives are a plausible start. Strings, symbols, nested values, and
  their Arrow encodings need separate representation decisions.
- Define how fixed-schema column children integrate with subscriptions,
  references, resampling, and output invalidation.
