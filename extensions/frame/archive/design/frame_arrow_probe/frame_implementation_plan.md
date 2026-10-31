# Frame extension: implementation status and remaining work

The 2026-10-05 [wiring materialisation proposal](wiring_materialisation_proposal.md)
updates the expression-wiring mechanism discussed below. It proposes BG changes;
those APIs are not implemented.

Status: consolidated on 2026-10-03. The
[working note](frame_design_note.md) records the current design direction;
the [entity strawman](frame_entity_strawman.md) gives proposed backing entities
and APIs. BG utility, scalar, and provider changes were integrated through
`b5a241f6`. The uncommitted extension is in `.lanes/frame-views/extensions/frame`.
It was built and tested against that committed BG revision. Main has since
advanced; the extension has not been rebased or integrated. Names and
signatures remain provisional. The extension
[README](../../../frame-views/extensions/frame/README.md) documents current APIs.

## Completed

| Area | Delivered |
| --- | --- |
| BG utilities | Live-run enumeration and typed/runtime sparse-bunch block access, integrated in `ffb1e3fc`. |
| BG scalar extension APIs | Trait-selected read values and supported alternative-runtime descriptors, integrated through `3bf46d07` and `b5a241f6`. |
| BG time-series extension APIs | Provider-owned types/runtimes, registration, typed and erased bindings, and default/view output specifications, integrated in `b5a241f6`. |
| Scalar values | Nullable `i64`/`double` columns, fixed-schema frames, borrowed read views, and stable owning snapshots. |
| Incremental series | Native `TSC`/`TSF` row insertion, patching, removal, clear/invalidation, row changes, and per-column changes. |
| Row storage | Recycled 32-bit IDs independent of user keys, removal-cycle ID retirement, physical slot reuse, and adjacent-batch coalescing. |
| Views and nodes | Borrowed TSC binding, TSF projection, constant numeric filtering, column selection, and independent column-child notifications. |
| Arrow values | Optional adapter, retained Arrow-backed scalar runtimes, invocation-scoped borrowed export, and dense owning gather-copy export. |
| Measurement | Real source → filter → column → sum graphs across six selection/deletion cases and three consumption paths. |

## Verification and performance evidence

- The integrated BG provider changes passed 1,133 tests at `b5a241f6`.
- The extension passes five test executables with Arrow enabled, four with
  Arrow disabled, and five under AddressSanitizer/UndefinedBehaviorSanitizer.
  Leak detection is disabled because of the environment's ptrace restriction.
- Tests cover row/column changes, view sharing, invalidation/revalidation,
  allocation-failure cleanup, snapshots, Arrow slices/nulls/chunks, and owning
  lifetime. A BG node consumes an Arrow-backed frame scalar.
- All benchmark sums matched deterministic expected values. The
  [report](../../../frame-views/extensions/frame/benchmarks/arrow_benchmark_results.md)
  and [CSV](../../../frame-views/extensions/frame/benchmarks/arrow_benchmark_results.csv)
  distinguish source mutation, whole-graph evaluation, consumer work, and a
  prebuilt dense Arrow kernel. Export timing includes allocation and destruction.

The benchmark extension uses Release; its installed BG library is Debug.
Both native and Arrow sums scan the current selected values. Fragmented Arrow
export plus sum is expensive; native view consumption was faster than dense
gather plus sum in the measured cases. The benchmark does not isolate chunk
construction from summation over an already-built fragmented Arrow array.
It establishes no automatic export threshold. Further performance tuning is
deferred for now.

## Outstanding functional work

1. **Computed columns and typed kernels.** Add outputs that follow the input
   row domain, arithmetic kernels, Boolean columns/comparison results, and
   `with_columns` composition. Start with `(q.bid + q.ask) / 2`, including
   nulls, additions, removals, and column-specific changes.
2. **Expressions and changing scalar inputs.** Use BG's existing
   materialisation mechanism for expression wiring and reuse concrete TSC
   results. Add the separate expression-construction node for scalar time
   series inputs, then support `filter(q, q.size > min_size)` with
   `min_size: TS<i64>`.
3. **Previous-cell subscriptions and incremental aggregates.** Capture
   preceding cells only for columns demanded by consumers, through their
   storage owner. Define subscription activation/release and view rebinding.
   Implement an incremental sum covering modifications, removals, filter
   exits, and threshold-only membership changes. Current removal deltas carry
   IDs; previous/removed cell values are not retained by default.

These complete the first numeric example below. The detailed slice sections
retain their implementation requirements and verification criteria.

## Deferred work

- **Storage/performance:** deletion-gap compaction, capacity trimming, faster
  selected traversal, a cached fragmentation query, and a measured choice
  between borrowing and gathering. Existing batches can be counted without
  constructing Arrow objects; that still requires traversal. Fragmentation
  statistics and automatic export selection have not been implemented.
- **Further measurements:** Release BG, separate fragmented Arrow export and
  kernel costs, and previous-cell/expression workloads when implemented.
- **Cell types and schema:** strings, symbols, additional scalar layouts, and
  pivot columns represented as dictionaries of whole columns. Their detailed
  storage and schema APIs remain to be settled.
- **Operators:** broader filtering, sorting into outputs, grouping,
  aggregation, joins, and pivots.
- **External use:** Arrow delta representation and Python-facing frame APIs.
  Current Arrow exports contain table values only. Browser transport remains
  outside the current data-type implementation scope.
- **Library delivery:** API review, install/export packaging, compiler
  verification, and eventual extension integration. Frame work remains
  uncommitted in its lane.

The frame implementation must be a separate module consuming BG's extension
points. The utility additions below are agreed BG changes. Any other missing
BG extension facility requires a concrete report and discussion before changing
core or introducing a workaround.

## Design summary

| Area | Current direction |
| --- | --- |
| Values | `Column<T>` and `Frame<SCHEMA>` are scalars. |
| Incremental series | `TSC<T>` and `TSF<SCHEMA>` add row identity and changes. Their borrowed scalar views copy no cells. |
| Storage | Owning outputs mutate columns; view outputs select rows and bind column sources. |
| Identity | Library nodes use stable row IDs independent of physical slots. User keys are optional. |
| Changes | Shared membership additions/removals and per-column changed IDs; no per-cell timestamp matrix. |
| Previous cells | A consumer subscribes directly through `ColumnSource` to its storage owner, per required column. |
| Expressions | Operators build expression wiring records. Existing materialisation creates concrete TSC outputs when needed. |
| Changing scalar parameters | A separate ordinary node publishes a bound expression scalar. Frame nodes consume it through `TS`. |
| Kernels | Runtime types expose typed batches; operation implementations execute them. There is no runtime type per kernel. |
| Arrow | Compatible layouts share cell buffers. Chunk/validity construction and incompatible layouts can require allocation or conversion. |

The expression-construction node is a code-organisation choice. It gives
filter and other consumers a uniform expression input. Its adoption does not
depend on demonstrating a performance benefit.

## First complete example

Build toward this fixed-schema numeric graph:

```text
Quotes = { size: int64, bid: double, ask: double }

q = quote_source(events)
mid = (q.bid + q.ask) / 2
enriched = with_columns(q, mid=mid)
visible = filter(enriched, enriched.size > min_size)
total = sum(visible.size)
```

`min_size` is an ordinary `TS<int64>`. The example exercises owning storage,
column children, a materialised computed column, an expression-construction
node, a selected frame, and consumer-driven previous-cell capture. Use numeric
row handles for input events; ingestion business-key indexes are separate.

The goal is a working incremental graph with these contracts. Implemented
Arrow export and alternative Arrow scalar backing use the same value APIs.

## 0. Utility access for native columns

Status: complete and integrated. The
[utility API proposal](utility_api_proposal.md) supplies the block and
run information that native batch readers and Arrow adapters will share.

| Operation | Current status and action |
| --- | --- |
| `index::first_set(start)` | Already present for dynamic and fixed indexes; reuse it. |
| `occupancy_index::for_each_run` | Implemented and tested: allocation-free iteration over contiguous live position runs. |
| `sparse_bunch::block_containing` | Implemented and tested: raw block address and physical bounds, including unallocated blocks. |
| Matching `runtime_sparse_bunch` access | Implemented and tested with `runtime_item_layout`. |

Keep the block calculation on the existing block-storage owner. The descriptor
reports raw storage; it must not imply that null or dead positions contain
constructed values. Frame batch readers intersect live runs with block bounds
and apply cell validity separately.

Verify empty/dense/scattered occupancy, runs crossing bitmap words, block
boundaries, unallocated blocks, and blocks released after their final cell is
erased. Put these tests in the existing utility test areas. The accessors must
not allocate or construct cells.

Row-ID mappings, compaction across columns, and previous-cell subscriptions
belong to frame storage. Existing construction/destruction APIs suffice for
relocation. The utility proposal remains authoritative for the detailed API.

## 1. One numeric column in BG

Status: complete for `i64` and `double` with the ordinary guarantee.

The separate module implements immutable `Column<i64>` values, native sparse
column storage, borrowed batches, and typed `TSC<i64>` input/output access.
Scalar, provider, and native TSC graph tests pass against the installed BG
package. The native output follows the existing initialization contract
described below.
Guarantee variables use BG matching and resolution; unsupported resolved
guarantees are rejected.

Start with `Column<int64>` and `TSC<int64>`, supported by native row storage.
The first graph is a source, an ordinary TSC consumer, and an observer of its
borrowed scalar value. This establishes the type and storage integration used
by every later frame operation.

Deliver:

- Row identity, occupancy, ID-to-slot lookup, nullable cells, and per-cycle
  additions/modifications/removals using existing bunch/index utilities.
- `Column` scalar access, its native runtime, and owning scalar copies that
  stay stable after subsequent source mutation.
- `TSC` input/output access, native runtime, source publication, and a
  zero-copy `value_view()`.
- Borrowed batch reads using live runs, physical blocks, and cell validity.
- Type matching, runtime selection, and wiring through ordinary BG nodes.

Use integer cells first, then Boolean and floating-point cells before the
expression slice. Generic structural types and alternate runtimes must remain
possible; a numeric first implementation does not fix every column to one
C++ storage layout.

Verify values, nulls, additions, repeated writes, removals, valid empty values,
and initialization/rebuild under BG's existing lifecycle. Verify that source
and borrowed view share cells and that owning scalar publication remains
stable. Use value tests for storage and `test_graph` for node behaviour.

### Native column implementation review

The installed provider package includes the agreed run and block APIs.
The provider lane also implements the approved scalar read-type correction:
typed scalar, time-series, and collection reads use
`scalar_storage_traits<T>::value_type`. The competing internal mapping has
been removed. Built-in read types and default construction remain supported.

With that correction, a provider-defined borrowed column read object can
expose cells and batches through the existing `Scalar::operator->()`. Creating
an owning scalar from mutable storage copies current cells at the ownership
request. Reads and ordinary row writes do not take that snapshot.
Verify both typed `ScalarValue<Column<T>>` and erased `ScalarValue<>` copies:
an erased copy uses the source runtime, whose owning construction must also
produce stable storage.

`TSS` and `TSD` reset change records and reclaim retired entries before their
first mutation at a later evaluation time. The native TSC can use the same
lifecycle without a new cycle-callback facility. Its default removal record
requires IDs only; it does not require retained cell values or keyed-row
restoration. Use the approved 32-bit internal IDs and retirement rules in the
[working note](frame_design_note.md#row-changes-and-identity). These IDs are
independent of optional user-defined key or index columns.

### Output initialization

Native columns follow BG's position-independent initial storage contract.
Their empty headers contain bunches, indexes, counters, and unbound scalar
descriptors. Mutations and cleanup receive the graph allocator. ID mappings
use existing bunch storage; initial headers capture neither an allocator nor
their own address. A successful initial mutation binds the scalar descriptor
to the actual runtime column before publication.

Immutable snapshots are allocated on an ownership request and can retain the
allocator used for their own storage. They are separate from the graph's
initial output image.

Verify allocation-free empty initialization, independent runtime instances,
correct graph-allocator cleanup, typed and erased snapshot ownership, and the
real TSC graph. This representation uses the existing BG initialization
contract and requires no core initialization change.

The graph tests verify valid-empty publication, row changes, invalidation with
live rows, and empty revalidation. Two runtime instances of one compiled graph
verify independent native storage and balanced allocator cleanup.

## Following implementation slices

### 2. Fixed-schema frames and column children

Status: complete in the extension lane.

Compose native columns over shared rows and membership changes. Add
`Frame<SCHEMA>`, `TSF<SCHEMA>`, owning row writes, and column selection as a
`TSC<T>` child. Supply direct ID reads and basic batch reads/writes.

Verify that changing `ask` schedules its child consumers while preserving
unmodified column change state. Verify shared additions/removals, complete
row insertion, explicit nulls, and omitted patch fields. Row additions and
removals must appear consistently on every column child.

### 3. Views and typed batch kernels

Status: projection, constant numeric filtering, and borrowed batches are
implemented. Computed columns, arithmetic kernels, and Boolean columns remain.

Implement projection, a computed TSC that follows input rows, and selected
membership over shared column sources. Start with numeric arithmetic and a
constant comparison. Establish the shared expression executor's batch access
without requiring deferred expression wiring yet.

Use the native batch reads established by the utility and column slices.
Automatic compaction policy remains later work.

Verify pointer sharing, source-order selection, alignment rejection, null
predicates, membership entry/exit, and payload-only changes. Nodes must
initialize/rebuild from current values and propagate relevant source changes.

### 4. Expressions through existing wiring

Status: design direction agreed; implementation remains.

Implement row-wise expression records, typed operator resolution, and
materialisation into computed TSCs. A filter graph overload consumes an
expression record to build a scalar expression value or a separate
expression-construction node for changing scalar inputs.

Use the existing materialisation cache for concrete TSC outputs. Keep the
column-result record and the builder's `TS<Expression<...>>` output as distinct,
correctly typed wiring records. Make source column dependencies explicit.

Verify the unchanged public syntax `filter(q, q.size > min_size)`. A threshold
tick evaluates all current rows but publishes only resulting membership and
payload changes. Column-only updates do not run the expression-construction
node. Test simultaneous threshold and frame updates, constants, nulls, and
reuse of an already materialised expression.

Defer sharing independent of wiring-call order until the integration point
in compilation/lowering is clear. Expression correctness must not depend on
this optimization. Begin with precompiled batch kernels and reusable scratch;
machine-code generation and automatic retained-intermediate selection are
later work.

### 5. Previous-cell subscriptions and an aggregate

Status: design direction agreed; implementation remains.

Add per-column demand at storage owners, first-previous-cell capture, and
subscription-backed previous reads. Keep the owner reference on
`ColumnSource` from the view slice so this requires no graph traversal.
Selected views supply previous membership.

Use `sum(visible.size)` to exercise updates, source removals, and filter exits.
Test a value change that also removes a row from the filter: subtract its
preceding selected value. Test threshold-only exits, where source cells did
not change. New rows have no previous value; nulls retain their null state.

Verify zero capture for undemanded columns, shared capture for multiple
consumers, first-write coalescing, and demand activation/release. A computed
column captures its own previous results without requesting previous inputs.
Complete this contract before describing the first example as finished.

### 6. Numeric Arrow boundary and measurements

Status: value imports/exports and the first graph measurements are complete.
Further measurements are listed under deferred work above.

Add an Arrow scalar representation and borrowed numeric export through the
same `Column`/`Frame` interfaces. Verify cell values, logical order, nulls,
shared compatible buffers, and explicit gather/conversion paths. Handle
all-null unallocated blocks through the adapter.

Keep Arrow dependencies in an optional adapter target. Native and Arrow
representations use the same typed cell and batch reads. The extension's read
source interface supports other representations; its batch validity view
supports BG indexes and bitmaps with a bit offset. These are extension
implementation APIs.

Borrowed export shares compatible value spans for the current node invocation
and allocates chunk metadata and validity bitmaps as required. Fragmentation
does not trigger an implicit gather. An explicit owning export gathers current
values into one fresh dense chunk per column and initializes bytes at null
positions. It remains valid after source mutation or destruction, including
when copied from an Arrow-backed input. Imported
Arrow scalar runtimes retain their arrays or tables. Typed owning scalar
copies follow BG's canonical storage traits; erased owning copies preserve
the source runtime.

The first measurements compare native batch consumption, borrowed Arrow
export plus a reduction, and dense owning Arrow export plus that reduction.
Use the same row changes and predicates for each path. Measure source mutation
separately from producer-to-consumer graph latency. Include export allocation
and destruction, report chunk counts, and cover clustered/scattered selection
and deletion at several densities.

After previous-cell subscriptions and expressions are implemented, extend
these measurements to demanded-column fractions, capture versus consumer
caches, and expression-construction nodes. Preserve the selected division of
responsibilities when optimizing those implementations.

## BG integration and library boundary

The extension lane uses committed BG `b5a241f6`. Preserve the original probe
and these uncommitted design files separately from implementation checkouts.

The relevant existing seams are:

- `WiringRecord::materialize` and the record/type cache in
  `src/wiring_record.cpp`.
- Ordinary graph/operator wiring, `const_`, node input resolution, and
  node-call reuse.
- Shared schema and column-child conventions already used by bundles.
- `Scalar`/`ScalarValue` ownership and scalar runtime dispatch.
- `TimeSeriesRuntimeType` item access, source publication, and explicit output
  runtime selection.
- Existing allocator, scratch, bunch, and index utilities.

Exercise type visitors and runtime resolution from the separate module and
report any missing provider or dispatch contract with a minimal reproducer. Discuss proposed BG changes
before implementing them. New frame types must use genuine extension support;
representing them as unrelated builtin types or relying on undocumented
internal specialisations does not satisfy the module boundary.

Main includes the supported provider contract, default runtime registration,
erased visitor fallback, and default/view output specifications. The integration
passed all 1,133 BG tests. The extension consumes an installation of committed
main. Its
`bg_frame_provider_integration_test` covers external declarations and views,
runtime registration, erased visitation, and a real producer/consumer graph
without internal BG APIs. The column and provider tests pass against the
updated installed package. The native TSC graph test now runs through the
same package and existing output initialization path. The provider tests cover
`TimeSeriesState` transitions, callable and aggregate
default/view outputs, default storage after view inputs, and view invalidation.
The provider fixture uses copied alternative storage. Frame view tests verify
shared cells, selected membership, and independent column notifications. The
column scalar tests verify shared buffers and owning lifetime.

The authorized BG scope is public declaration and variable-enumeration
customizations, public runtime customization and registration, supported
input/output bindings, selected-runtime preservation in node binding, erased
time-series visitation, and public generic scalar declaration metadata.
The implementation must exercise the contract with an external provider,
generic node signatures, and an external series contained in a built-in
collection. BG core remains independent of frame declarations and storage.

`ScalarData` and `ScalarDataMode` descriptors are authorized as APIs for
extension authors implementing alternative scalar runtimes. Node code uses
scalar and time series views. The descriptors' layout and behavior stay
unchanged; provider-facing `ScalarRuntimeType` signatures use the supported
names from `<bg/scalar/data.hpp>` in the provider lane.
Output specifications are authorized: `Out<TSF<SCHEMA>>` selects the default
writable representation, and `Out<TSF<SCHEMA>, view>` selects a view
representation. The same rule applies to `TSC`. The specification belongs to
the output declaration and does not change the semantic time series type.
Runtime selection uses the resolved output type and the output specification;
it receives no input runtimes. View source bindings retain the runtime
information needed to read their sources. Callable and aggregate node binding
must preserve the complete output declaration. A default mutable frame output
must select writable storage even when its input is a view.

The module location is `extensions/frame` in the `frame-views`
lane, with an independent `bg_frame` target. Frame storage, kernels, expression
construction, and nodes belong there. Arrow-specific dependencies belong in
its adapter. Utility changes are integrated in main; the original probe and
design files remain in `frame-arrow-probe`.

Four semantic families and their runtime interfaces are implemented: `Column`,
`Frame`, `TSC`, and `TSF`. `Expression` additionally needs a scalar declaration
and storage suitable for ordinary `TS`; it adds no time-series runtime family.
Begin with existing scalar facilities for that value where possible.

## Choices required before their implementation slice

Native storage uses 32-bit recycled IDs and nullable cell reads. It reuses
physical slots independently of row-ID retirement. Insertion order is not
guaranteed after removals; views follow the source's current order.
Before expressions: define the wiring
adapter and the expression scalar's type parameters. Before previous capture:
define subscription activation and the capture/read API.

These are bounded API choices. Automatic compaction thresholds, string and
symbol layouts, pivots, sorted-output order, general joins, arbitrary expression
fusion, and browser transport can follow the first complete numeric graph.
