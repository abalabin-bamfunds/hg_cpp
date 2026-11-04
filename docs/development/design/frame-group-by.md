# Grouped aggregation

## Public API

Grouping describes a frame and its grouping columns. Aggregation materialises
that description as an incremental frame:

```cpp
auto grouped = frame::group_by(quotes, "symbol");
auto totals = grouped.agg(
    "quantity"_n = operators::sum_(col("size")),
    "notional"_n = operators::sum_(col("price") * frame::convert<double>(col("size"))));
```

The first implementation groups by existing column names and provides numeric
sums. Callers can calculate a grouping column with `with_columns` first. Names
and aggregation assignments can also be supplied as wiring-time collections.
Source and result schemas use the same implementation whether their fields
come from C++ declarations or wiring-time registration.

## Wiring

`group_by` returns a typed wiring point over an ordinary free TSB. Its fields
retain the source frame and grouping names. `wiring_point_methods` supplies
`.agg` by forwarding to the registered `agg` operator. A Python frame binding can
register the same operator through `hg::python::register_wiring_method`; this
change does not add a Python frame module.

The `sum_` expression overload returns a free TSB that retains the aggregation
description and its operand. This preserves scalar expressions and captured
time series for inspection during wiring. The existing numeric TSC overload
continues to produce a scalar time series.

`agg` resolves operands against the source schema through the existing column
expression operators. It infers an anonymous output schema containing the
grouping columns followed by the named aggregate columns. The grouping columns
form the output's ordered key tuple. Existing frame key rules apply: null cells
compare equal and key tuples are unique.

`agg` infers the output schema immediately. Its materialisation builder compiles
bound operands after BG records materialisation requirements. It uses the same
compiler as column arithmetic and filtering. A materialised intermediate becomes
a leaf column; other expression operations execute inside the aggregation node.
Each aggregation retains its leaf-column and scalar-parameter dependencies.

The node consumes the source frame and bundles of leaf columns and parameter
values. It evaluates operand blocks and applies the selected aggregation kernels
in one node. No computed operand column is required.

## Aggregation kernels

The extension-facing API is in `hg/frame/aggregation.hpp`. An
`AggregationExpression` retains an operand and an `AggregationOperation` scalar.
The operation identifies a `ScalarDispatch<AggregationFunctionAdapter>` registry.
Wiring looks up the resolved operand cell type. The selected adapter supplies the
result scalar type and constructs one aggregation state. Registries must remain
alive until wiring finishes.

Registration follows the column block function interface: implementations use
typed batches, registration generates an erased adapter, and wiring selects the
adapter before evaluation. Register a typed implementation with
`register_aggregation_function<RESULT, ARG, STATE>(registry)`. The state constructor
receives an `allocator`. Registration returns `false` when the registry already
has a handler for `ARG`; it preserves that handler. Its block operations receive
row IDs and aligned group positions:

- `update(rows, groups, const ColumnBatch<ARG>&)` replaces the contributions of
  the supplied rows.
- `remove` removes cached contributions without reading source cells.
- `move` transfers cached contributions between groups.
- `add_group` and `erase_group` manage group state.
- `result` returns `cell_value_t<RESULT>` or `std::optional<cell_value_t<RESULT>>`.
  The adapter owns the returned scalar storage until its next result call.
  `AggregationRows` provides the node-owned membership mapping for kernels that
  need a rescan.

Implementations own group state and cached row contributions; they do not derive
from the erased interface. Input batches and membership views are borrowed for
their calls. Column block functions write a result batch into caller storage;
aggregation implementations retain state across blocks and publish one scalar
per group. Both interfaces use `ColumnBatch<T>` and BG's scalar storage traits.

Grouping contains no numeric reduction dispatch. The `sum_` expression overloads
contribute the sum registry. Its integer and floating-point implementations use
the same arithmetic as standalone column sums.

## Row changes

The node retains each source row's group. Each kernel retains its contributions.
`In<TSF>::changes()` and `In<TSC>::changes()` expose the input's current change
bitmaps. Framework input state determines whether those bitmaps are observable.
Nodes do not compare storage binding addresses or update timestamps.

Additions and grouping-column changes read the grouping columns. Removals use
cached membership. A changed key moves cached contributions only if its group
changes. Operand-only changes use the cached group position and update only the
affected aggregation. A scalar parameter update evaluates all current rows for
its aggregation. Ordinary column updates process the word-wise union of their
added and modified bitmaps; each row is emitted once, without sorting.

Physical compaction preserves row IDs. Same-domain column rebinding reports
modified cells. Keyed reference sampling matches complete user keys and exposes
surviving-row ID mappings through the input API. Group-by removes cached
contributions for removed and remapped old IDs before processing current IDs.
Sampling marks all surviving cells modified, so this also reconstructs membership
for remapped rows without adding a remapping operation to aggregation kernels.
Unkeyed source replacement uses removals and additions. Removed source cells are
not read.

Each nonempty group has one output row. Its row ID remains stable while the
group exists. Groups that are empty after applying the invocation's changes
are removed. Output order is unspecified. Source replacement preserves output
rows for groups that remain nonempty. Invalidation discards retained membership
and contributions.

Grouped and column sums share the
[numeric sum rules](../../user/extensions/frame-grouping.md#numeric-sum-rules).
Finite double contributions use Neumaier compensation, including negative
contributions on removal. Finite overflow recovery rebuilds all affected groups
in one traversal of the current rows. Publication reuses those results until
another mutation requires recovery.

## Verification

Catch2 graph tests cover additions, numeric updates, removals, changes to
grouping columns, composite and nullable keys, strings and symbols, group row
lifetimes, row-ID reuse, and invalidation followed by revalidation. Tests also
cover wiring-time schemas, multiple sums, captured scalar time series,
expression operands, and wiring errors. Both column and grouped reductions use
the same numeric sum implementation. Tests cover floating-point overflow,
infinities, NaN, and recovery after updates.
