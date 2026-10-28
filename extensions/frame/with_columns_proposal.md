# Adding and replacing frame columns

Status: approved design; inferred-schema column assignments are implemented.

The current implementation accepts concrete `TSC<T>` inputs, column expressions,
scalar constants, and scalar time series. Explicit concrete output schema
validation and shared-materialisation profiling remain future work.

Combined BG and Arrow-on frame Release verification passes all nine frame CTest
executables. The test that resolves the inferred schema inside an enclosing
graph template also passes after BG fixed named-argument and helper forwarding
lifetimes. See [HANDOFF](HANDOFF.md) for the exact verification and audit
findings.

## Purpose

`with_columns` adds or replaces columns while preserving the source frame's
row IDs, row domain, visible row sequence, and key. It borrows column storage
from its inputs. Expression computation uses the existing BG nodes and
[block-operation implementation](block_operation_proposal.md).

## Graph API

Inferred output schema, including when the input schema is supplied during wiring:

```cpp
auto priced = frame::with_columns(
    q,
    "mid"_n = (frame::col("bid") + frame::col("ask")) / 2.0);
```

The returned wiring point carries a resolved anonymous output schema.
Runtime assignment names use a span of named `WiringArgument` values, following
BG's existing dynamic argument API.

An optional concrete output schema will validate the calculated result in a
future stage:

```cpp
auto priced = frame::with_columns<EnrichedQuotes>(
    q,
    "mid"_n = (frame::col("bid") + frame::col("ask")) / 2.0);
```

The current API provides the inferred-schema form. Both forms are intended to
use the same schema calculation and runtime node.

An assignment accepts an unbound expression, a bound column expression, a
concrete `TSC<T>`, or a scalar constant or scalar time series of a supported cell
type. Scalar assignments broadcast over the source frame's visible rows.
Expressions can also capture scalars through the existing expression API.

```cpp
auto enriched = frame::with_columns(
    q, "venue"_n = symbol{"LSE"}, "limit"_n = limit_ts);
```

All assignments read the input frame. An expression cannot refer to a column
introduced by another assignment in the same call. Chain calls to express that
dependency. Duplicate assignment names are wiring errors.

An assignment replaces a source field with the same name or adds a new field.
The inferred schema preserves source field order and appends new fields in
assignment order. Replacement uses the expression's exact result type; it does
not insert a conversion. A concrete output schema must contain precisely these
fields with matching types; its declaration determines output field order.

The inferred schema preserves source schema metadata. An explicit output schema
supplies its own metadata and must preserve the same ordered key field names.
Key columns cannot be assigned.

## Ownership and graph construction

Use an ordinary frame assembly node with a borrowed output:

```text
source TSF -------------------------------+
                                          |
bid, ask -> existing expression node -> mid TSC
                                          |
                              frame assembly node
                                          |
                                 enriched TSF view
```

The assembly node borrows unchanged fields from the source frame and assigned
fields from concrete column inputs. It owns membership and change information,
as existing frame views do. It does not own copied cell buffers.

The assembly node declares independent source, assignment, and result schema
variables. A wiring resolver calculates the result schema from the resolved
source frame and assignment TSB. The ordinary view output declaration selects
its runtime layout after this resolution. One node definition handles all
schemas; user schema declarations do not instantiate separate node definitions.

BG materialises each assigned result as a `TSC<T>`. The existing expression
compiler fuses upstream operations until a concrete materialisation is
required. A result already required elsewhere is shared with the assembly node.
Two assignments using the same wiring point share that result.

Distinct assigned results use separate expression nodes. This preserves their
independent dependencies and incremental evaluation. Shared operations inside
distinct expressions follow the existing BG materialisation rules.

Scalar assignments use an ordinary node with a `TSF` source, a `TS<T>` value,
and an `Out<TSC<T>, frame::computed<>>` output. Constants use `operators::const_`.
The computed output owns its cells and follows membership directly
from the frame. The [node API](README.md#adding-and-replacing-columns) defines
incremental writes, invalidation, and supported cell types.

## Frame binding change

Extend the existing `Out<TSF<Schema>, frame::view>` binder to accept column
inputs from an ordinary TSB, alongside the source frame that supplies membership.
Proposed node-author operation:

```cpp
output.bind_columns(source, assigned_columns);
```

The TSB field names identify assigned output fields. Other fields borrow their
same-named source columns. Wiring fixes field ordinals and typed adapters.
Concrete and runtime schemas use the existing frame view runtime and storage.
Existing `computed<...>` outputs remain the API for nodes writing owned frame
columns directly.

Each assigned column must expose the source's exact visible row selection,
using the same descriptor contract as existing expression inputs. Matching
numeric row IDs alone does not establish compatibility.

Generalise the current frame binder's per-column binding and modification
handling to read each column's own input activity and delta timestamp. Source
membership controls additions and removals. Source field changes propagate only
to borrowed source fields; assigned column changes propagate only to their
assigned fields. Binding changes and unavailable deltas follow the current
frame-view reconciliation rules.

The output borrows owners retained by ordinary BG input connections. Invocation
descriptors remain invocation-local. BG owns signature resolution and
materialisation; the frame extension owns schema calculation and column binding.

## Implementation and verification

Implemented stages generalise borrowed-column binding and add wiring-time schema
calculation with inferred output and dynamic assignment names. BG resolves the
graph output schema from the selected graph result after the graph body runs
once. Combined Release verification passes all nine frame CTest executables.

Outstanding stages add concrete output schema validation and complete the
remaining binding and materialisation checks. Verification must cover concrete
and runtime schemas, replacement, keys, nulls, source additions and removals,
independent column updates, scalar captures, rebinding, and invalidation. Verify borrowed buffers and row domains
are shared. Verify that shared expression results materialise once, then measure
the expression and assembly nodes separately against the existing expression
path.
