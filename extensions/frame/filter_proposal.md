# Expression-based frame filtering

Status: implemented. The frame overload uses BG's existing filter operator,
the shared expression compiler, and the TSF view output operation.

## Purpose

Filter rows with the existing [column expression contract](block_operation_proposal.md).
The output owns row selection and change information and borrows source columns.
The expression compiler supplies predicate blocks to the output.

## Graph API

The convenience functions accept a bound predicate or bind an unbound
expression to a frame:

```cpp
auto selected = frame::filter(q, col("size") > bg::i64{10});
auto changing_threshold = frame::filter(q, col("size") > min_size);

auto predicate = bind_expression(q, col("ask") - col("bid") < tolerance);
auto selected_spreads = frame::filter(q, predicate);
```

Direct operands use the existing expression capture support. A scalar is owned
by the expression; a scalar time series is an ordinary BG dependency whose
updates reevaluate the predicate for all current rows. Named `param("name")`
placeholders remain available for expressions bound to different arguments.

Both functions call BG's `operators::filter_(predicate, source)`. The frame
extension supplies the overload for row predicates. Its predicate has semantic
type `TSC<bool>` and can retain its expression materialisation option. A scalar
`TS<bool>` has BG's existing tick-selection semantics and is not a row predicate.

The output retains the source schema, key metadata, row identities, and row
traversal order. A valid null predicate excludes its row, as does false. Only
true selects a row. Comparison callbacks return null when either cell is null.
Operand cell types must match; mixed types fail during wiring. Explicit column
conversion remains later work.

## Wiring

The frame overload declares its predicate as a graph-only union of `TSC<bool>`
and the existing expression bundle form. It creates one deferred frame output
with one materialisation builder. The builder declares the frame dependency
and keeps the predicate union open until BG has recorded concrete demands.

The builder uses the existing expression compiler. It reuses a concrete
predicate or intermediate when BG has required its materialisation. Otherwise,
it compiles the predicate into the filter node's block calls. Compilation
deduplicates column and scalar inputs and wires the existing parameter
collector. Captured scalar time series remain ordinary BG dependencies.

The compiled expression records its result operand explicitly. A result can
refer directly to a column batch or to an instruction's batch. This is required
to filter with a materialised `TSC<bool>` without adding an identity operation.

## Node

The filter node receives the immutable compiled expression, the source frame,
the concrete column bundle, and the scalar parameter snapshot. Its output is
`Out<TSF<SCHEMA>, frame::view>`. Its binding cache follows the existing
[expression node design](expression_node_proposal.md).

At invocation, the node performs these operations:

1. Prepare column bindings and scalar broadcasts with the shared expression
   input preparation code.
2. Require each predicate column's visible `RowSet` descriptor to equal the
   source frame's descriptor. This compares descriptors; it does not scan rows.
3. Determine whether the predicate needs a full rebuild. Output invalidity,
   changed predicate bindings, lost delta continuity, and parameter updates
   require a full rebuild. Otherwise, collect changed predicate row IDs.
4. Call the output's complete filter operation with that information and a
   block evaluator.
5. Save successful binding fingerprints.

An update to an unrelated source column schedules payload forwarding without
reevaluating the predicate. A parameter update reevaluates all current rows.

## Output operation

The node-facing operation has this form:

```text
output.bind_filter(source, rebuild_predicate, predicate_changed_rows, evaluator)
```

`predicate_changed_rows` is a borrowed span of row IDs. `evaluator` receives a
requested `RowSet` and a consumer for `(row_ids, ColumnBatch<bool>)` blocks.
Every requested row must be supplied once. The evaluator and consumer are valid
only during this call.

`RuntimeFrameViewOutputStorage` owns the complete transition:

1. Bind the source and reconcile source membership using its existing delta
   and replacement rules. Remove departed rows from the output selection.
2. Combine source additions with changed predicate rows. Source replacement
   or lost source delta continuity can also require a full predicate rebuild.
3. Invoke the evaluator over the requested current rows. Include each row with
   a valid true predicate; exclude each row with a false or null predicate.
4. Rebuild selected traversal order when membership changes.
5. Bind borrowed columns to the resulting selection and forward selected
   payload modifications.
6. Publish changed child columns and the frame after evaluation completes.

The operation reuses the existing selection, selected order, and delta storage.
It replaces the single-column cell callback prototype. Nodes do not maintain
a second selection or publish partially processed predicate blocks.

## Verification

Verify direct boolean predicates, fused comparisons, and reuse of concretely
materialised predicates. Cover scalar constants and changing time series
parameters, null exclusion, source additions/removals, predicate changes,
unrelated payload forwarding, reference replacement, invalidation/revalidation,
and row ID recycling. Verify concrete and runtime schemas use the same path.
Check that borrowed output columns share source value buffers, and that rows
retain source traversal order when predicate changes alter membership.
