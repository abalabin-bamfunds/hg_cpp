# Frame projection and standard library integration

Status: proposed. This note defines implementation stages for review.

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

For computed selections, reuse `with_columns` expression binding, broadcasts,
and column assembly, emitting only requested fields. Borrow selected source
columns and materialise computed columns through existing BG operators.

Keep `project<TargetSchema, Names...>` as a wrapper over the same projection
implementation, preserving its explicit target-field mapping.

Add the planned `with_columns<TargetSchema>` form using the existing schema
calculation and assembly node. Validate exact field names and cell types;
the target declaration controls output order and metadata. Follow the
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
| `len_`, `is_empty` | Add row-count and empty-row-set overloads for TSF and TSC. |
| `valid`, `all_valid`, `count` | Verify generic implementations. Null cells do not invalidate a column. `count` continues to count root ticks. |
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
- `copy(input, output)` copies the complete current value and reconciles the
  destination, including rows removed while forwarding was suspended.

Destinations must retain their published cells independently of future source
mutation. Binding borrowed columns is insufficient for sampling, closed gates,
or a completed `take`. Read native, view, and computed sources through their
input interfaces and write owned output storage. Reuse stored string handles
where the existing storage API permits; do not require copying text bytes as
part of the contract.

### Row identity decision before implementation

A copy needs an explicit rule for destination row lifetimes and source-ID
mapping. A source row ID is reusable after its removal cycle. For example:

```text
sample: source row 0 contains A
skip:   remove source row 0
skip:   reuse source row 0 for B
sample: source row 0 contains B
```

The second sample must not infer that A and B are the same row from the numeric
ID alone. Key columns cannot supply the universal solution because frames can
be unkeyed. Review the continuity rule and destination identity before
implementing `copy`, replay, or operators that skip updates. The first
implementation need not provide minimal deltas after a gap.

Retained ticks already have frame write and discard handlers for test capture.
Add replay through `TimeSeriesTickOperations`, sharing their encoding and
ownership. Replay needs a mapping from recorded row lifetimes to destination
rows; it must handle numeric ID reuse in event order. Capture support alone
does not establish `delay` support.

## BG extension points to review

`SampledReferenceChange::create` currently constructs samples for built-in
TSB, fixed TSL, TSS, and TSD types. It has no frame-provider sampling path.
TSF's fixed columns do not by themselves describe row additions/removals for
its TSC children.

First produce a failing reference-switch case using public graph APIs. Then
propose the smallest provider-facing BG change required to expose frame
sampling correctly. Discuss that core change before implementation. Do not
add frame-specific branches to BG or reproduce reference routing in bg-frame.

The existing runtime copy hooks and tick replay registrations already provide
the entry points for copying and delay. Additional core facilities require a
concrete missing operation.

## Implementation order

1. Review the projection syntax and key inference rule. Implement single-column
   standard selection and schema-driven select/rename/drop. Use one projection
   node for borrowed columns and existing expression assembly for computed
   selections. Include explicit `with_columns` output validation.
2. Add standard row-count queries and verify generic validity/tick-count
   behaviour. Exercise composition using public graph APIs and anonymous schemas.
3. Review the copying row-identity contract. Implement copy operations once in
   the frame runtimes, then verify the standard tick-selection and timed nodes.
4. Review and implement the required provider reference-sampling extension;
   verify reference selection and nested graph boundaries.
5. Add retained-tick replay and verify standard `delay`.
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
