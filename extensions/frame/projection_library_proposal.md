# Frame projection and standard library integration

Status: projection and row-count queries are implemented. Copying, retained-tick
replay, and formatting integration are in progress.

## Purpose

Provide ordinary graph-code operations for selecting, renaming, and dropping
frame columns, including schemas defined during wiring. Integrate TSF and TSC
with BG's existing operators through their provider interfaces.

The [frame API](README.md) defines current storage, row identity, lifetime,
expression, and mutation contracts. This proposal retains those contracts.

## Projection API

Use Polars' names and call forms, adapted to BG graph code:

```cpp
auto prices = frame::select(q, "bid", "ask");
auto labelled = frame::rename(prices, {{"bid", "best_bid"}});
auto trimmed = frame::drop(q, "venue");

auto displayed = frame::select(
    q, "symbol", "mid"_n = (frame::col("bid") + frame::col("ask")) / 2.0);
auto enriched = frame::with_columns(
    q, "mid"_n = (frame::col("bid") + frame::col("ask")) / 2.0);

auto bid = q["bid"]; // Existing BG getitem operator, supplied by bg-frame.
```

`select` returns only the requested columns and named expressions.
`with_columns` retains existing columns and adds or replaces assignments.
Named arguments provide Polars' keyword-expression form. Expressions read the
source frame; chain calls to reference a newly assigned field.

`rename` takes an old-name to new-name mapping, matching Polars. Renames apply
simultaneously, so swapping two names is possible without an intermediate name.
`drop` removes columns. Missing source names are wiring errors, matching Polars'
default strict behaviour.

All names are scalar wiring arguments. A runtime collection of names supports
configuration-driven projection; it reaches the same implementation as the
ordinary call. Column names and the output schema remain fixed during graph
evaluation. `q["bid"]` returns a TSC; `select(q, "bid")` returns a TSF.

Positional strings in `select` identify columns. Named assignments retain
`with_columns` semantics: a scalar string is a value; `col(name)` explicitly
references a column. Polars also interprets named strings as column names;
this difference preserves BG's existing scalar assignment contract.

Start with column names, named expressions, and runtime name collections.
Expression aliases and inferred expression names, selectors, callable renames,
and Polars' optional non-strict forms can extend this API separately. Selection
uses the existing expression and materialisation implementation.

### Schema calculation

- Read the actual resolved source schema, including fields beyond a Base
  annotation. C++ declarations and wiring-time schemas use the same calculation.
- `select` emits fields in argument order. `drop` preserves retained
  field order. `rename` preserves field positions and exact cell types.
- Reject missing names, repeated removals, repeated rename sources, and
  duplicate output names during wiring. A source column may appear under
  several distinct output names.
- Selecting no columns produces a zero-column frame with the source's rows.
- Produce an anonymous schema and preserve applicable source metadata.
  Transform the frame key metadata as described below.

Preserve the complete ordered key tuple when all its fields remain borrowed;
rewrite its names for renames. If any key field is omitted, produce an unkeyed
output. This selects the output schema according to the existing
[keyed view contract](README.md#tsf-inputoutput-surface), without a value scan.
When a key field has several borrowed aliases, use its first selected alias.
The output key tuple retains the source key order.

### Runtime implementation

One generic projection node declares independent source and result schema
variables. Wiring calculates the result schema and source-column ordinals in
result-field order. Evaluation calls the existing
`Out<TSF<RESULT>, frame::view>::bind_projection` operation.

Selecting existing columns, renaming, and dropping share that node and the
existing borrowed runtime. They preserve source rows, traversal order, row IDs,
and the exact row selection descriptor. Only mapped-column changes publish
child modifications. No separate representation is introduced for wiring-time
schemas.

Computed selections reuse `with_columns` expression binding, broadcasts,
and column assembly, emitting only requested fields. Selected source columns
are borrowed; computed columns materialise through existing BG operators.

`project<TargetSchema, Names...>` wraps the same projection implementation,
preserving its explicit target-field mapping.

`with_columns<TargetSchema>` uses the same schema calculation and assembly node
and validates exact field names and cell types. The target declaration controls
output order and metadata, following the
[approved explicit-schema rules](with_columns_proposal.md#graph-api).

## Standard operator ownership

The frame extension declares `select`, `rename`, and `with_columns` in
`bg::operators` and registers their graph implementations. It registers a
column-removal overload on the existing `bg::operators::drop`. These declarations
and implementations belong to the extension; they require no frame-specific
code in BG core.

The `frame::select`, `frame::rename`, `frame::drop`, and `frame::with_columns`
functions prepare C++ call syntax and invoke the operators. Preparation converts
name collections, packages named arguments, and supplies explicit target types.
The selected graph implementation owns schema calculation, expression binding,
key inference, and construction of projection or assembly nodes. Operator
dispatch provides normal overload selection and wiring diagnostics.

BG owns the other existing operator declarations. The frame extension registers
its overloads and implements its provider operations.

| Existing operator | Proposed TSF/TSC support |
| --- | --- |
| `getitem`, `getattr` | Resolve a scalar column name during wiring and select the existing child TSC; reuse the column-selection implementation. |
| Arithmetic, comparisons, Boolean operations, `convert` | Retain existing column overloads and block expressions. |
| `sum_` | Retain existing incremental numeric TSC reduction. |
| `len_`, `is_empty` | Count visible live rows or test for no rows in TSF and TSC. Null cells count as rows. |
| `valid`, `all_valid`, `count` | Use generic implementations. Null cells do not invalidate a column. `count` counts root ticks. |
| `sample`, Boolean `filter_`, `take`, `drop`, `step`, `throttle`, `resample` | Supply provider `copy` and `copy_tick`; reuse standard nodes where their contracts permit. |
| `if_then_else` and reference routing through nested graphs | Verify target switching, current values, row changes, and child notifications. Resolve the reference-sampling gap before claiming support. |
| `delay` | Add retained-tick replay using BG's existing tick-operation registration. |
| `dedup` | Separate design step: frame types currently accept only ordinary guarantees. |
| `merge`, `collect`, `emit` | Separate operation-specific designs; no row or column combination semantics are implied by this proposal. |

Standard `filter_(TS<bool>, q)` selects ticks and preserves its last published
value while closed. Existing frame `filter_(TSC<bool>, q)` selects rows.
Standard `take`, `drop`, and `step` count ticks. `frame::drop(q, names...)`
removes columns. The namespace and argument types distinguish the two `drop`
operations. These meanings follow BG's
[standard library contracts](../../docs/development/design/library.md).

## Copying and retained ticks

The existing [provider contract](../../docs/development/design/time-series-providers.md#runtime-types)
provides the two required runtime operations:

- `copy_tick(input, output)` applies current-cycle additions, removals, and
  modified cells. It does not reconstruct omitted earlier updates.
- `copy(input, output)` replaces the destination with the complete current
  value, including changes made while forwarding was suspended.

Destinations must retain their published cells independently of future source
mutation. Binding borrowed columns is insufficient for sampling, closed gates,
or a completed `take`. Read native, view, and computed sources through their
input interfaces and write owned output storage. Reuse stored string handles
where the existing storage API permits; do not require copying text bytes as
part of the contract.

### Destination row identity

A copy needs an explicit rule for destination row lifetimes and source-ID
mapping. A source row ID is reusable after its removal cycle. For example:

```text
sample: source row 0 contains A
skip:   remove source row 0
skip:   reuse source row 0 for B
sample: source row 0 contains B
```

Full `copy()` starts new destination row lifetimes for every current source
row. It removes previous destination rows and copies the complete current
value. The second sample therefore does not infer that A and B are the same
row from the numeric ID alone.

Consecutive `copy_tick()` calls preserve destination row lifetimes. The
destination owns the source-to-destination row mapping. On a current input
event, an uninitialized destination or a changed source row domain initializes
from the complete current value. An unchanged input does not publish.
Invalidation clears the mapping. A caller that suspends forwarding or switches
inputs uses `copy()` before resuming delta copies. Runtime descriptors do not
own this per-output state.

Callers follow the destination mutation contract in
[Copying time series](README.md#copying-time-series).

Retained ticks already have frame write and discard handlers for test capture.
Add replay through `TimeSeriesTickOperations`, sharing their encoding and
ownership. Replay needs a mapping from recorded row lifetimes to destination
rows; it must handle numeric ID reuse in event order. Capture support alone
does not establish `delay` support.

Retained ticks record the source row-domain identity. Replay and live
`copy_tick()` use the same destination mapping, including when a buffering node
switches between the two operations. The identity is an opaque token for
graph-local retention; readers do not dereference it. Replayed invalidation
clears the mapping before a later source lifetime can reuse an address.

## BG extension points to review

The scalar and time series integration audit covers these operation sets:

| Operation set | Current support and remaining work |
| --- | --- |
| Scalar semantic types, storage traits, alternative runtime reads | Implemented for Column and Frame, including actual derived frame schemas. |
| Scalar construction, assignment, ownership, equality | Implemented. Owning copies of mutable storage take snapshots; immutable snapshots can share owners. |
| Scalar hashing | Explicitly unsupported. |
| Scalar literals | Implemented for native and Arrow layouts. |
| Scalar buffer storage | BG's generic owning `ScalarValue` encoding applies; a specialized encoding is optional. |
| Scalar formatting | Implemented through `ScalarFormatOperations` for both scalar families and native/Arrow layouts. |
| Time series declarations, runtime selection, output options | Implemented for native, view, and computed layouts. |
| Publication, invalidation, validity, child access and notifications | Implemented; verify composition while adding copy support. |
| Typed input/output visitation | Implemented by forwarding to BG's erased views. |
| Time series copying | Implemented for matching schemas and inherited-prefix copying into Base destinations. |
| Time series literals and captured expectation comparison | Implemented for both families. |
| Retained ticks | Write, discard, and native replay are implemented. Public feedback and gate tests verify retention and replay/live transitions. |
| Live and retained tick formatting | Implemented through `TimeSeriesTickFormatOperations`. |
| Reference sampling | Implemented through BG's runtime sample factory for TSC and TSF, including inherited schemas and cycle-start row removals. |
| Queue literal serialization | BG's queue codec rejects provider-defined time series; supporting TSF/TSC requires a separate BG extension hook. |

Frame runtimes supply `TimeSeriesRuntimeType::reference_sample_factory()`.
[Reference sampling](README.md#reference-sampling) defines the resulting row
changes and child notifications.

The existing runtime copy hooks and tick replay registrations already provide
the entry points for copying and delay. Additional core facilities require a
concrete missing operation.

`TimeSeriesTickOperations::typed_replay<Out<TS_T>>` passes its existing erased
output directly to the callback. BG commit `ce14d7c0`, merged into main as
`3cc9691b`, provides this behavior. Provider replay tests
and the full frame suite pass against the matching BG build.

## Implementation order

1. Implemented: single-column standard selection, schema-driven
   select/rename/drop, and explicit `with_columns` output validation. Borrowed
   columns use one projection node; computed selections use expression assembly.
2. Implemented: standard row-count queries and generic validity/tick-count
   verification through public graph APIs, including anonymous schemas.
3. Implemented: native copying with output-owned row mappings and Base-schema
   copying; public sampling and tick-selection verification.
4. Implemented: retained-tick replay and scalar/tick formatting through the
   existing operation registries; public feedback and buffering verification.
5. Implemented: provider reference sampling, reference selection, and nested
   graph verification.
6. Review deduplicated guarantees separately, then extend the reduction and
   operator library according to application needs.

Each implementation stage includes review before the next stage starts.
Any BG change remains separate from frame implementation work.

## Verification

Projection tests use declared and genuinely anonymous source schemas, runtime
name collections, renamed keys, omitted keys, empty projections, invalidation,
row-ID reuse, and column-specific notifications. Verify borrowed buffers and
selection descriptors directly where required.

Copying tests must change the source between publications and read the held
output from an independently ticking observer. Cover updates skipped by gates,
removal and reuse of the same numeric ID, source replacement, null cells, and
invalidation. Verify capture names identify the intended destination lifetimes.

Replay tests retain each event, mutate its source, and replay additions,
modifications, removals, invalidation, and ID reuse into a separate output.

Keep standard operator semantics in BG's library documentation. Document only
frame-specific meanings and supported operations in the extension README.
