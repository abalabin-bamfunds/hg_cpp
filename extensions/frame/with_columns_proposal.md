# Adding and replacing frame columns

Status: approved design; implementation in progress.

## Purpose

`with_columns` adds or replaces columns while preserving the source frame's
row IDs, row domain, visible row sequence, and key. It borrows column storage
from its inputs. Expression computation uses the existing BG nodes and
[block-operation implementation](block_operation_proposal.md).

## Graph API

Concrete output schema:

```cpp
auto priced = frame::with_columns<EnrichedQuotes>(
    q,
    "mid"_n = (frame::col("bid") + frame::col("ask")) / 2.0);
```

Inferred output schema, including when the input schema is supplied during wiring:

```cpp
auto priced = frame::with_columns(
    q,
    "mid"_n = (frame::col("bid") + frame::col("ask")) / 2.0);
```

The inferred form returns `wp<TSF<SCHEMA>>` with a resolved anonymous schema.
Runtime assignment names use a span of named `WiringArgument` values, following
BG's existing dynamic argument API. Both forms use the same schema calculation
and runtime node.

An assignment accepts an unbound expression, a bound column expression, a
concrete `TSC<T>`, a scalar constant, or a scalar time series. Scalar assignments
broadcast over the source frame's rows. Expressions can capture scalar constants
and scalar time series through the existing expression API.

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

BG materialises each assigned result as a `TSC<T>`. The existing expression
compiler fuses upstream operations until a concrete materialisation is
required. A result already required elsewhere is shared with the assembly node.
Two assignments using the same wiring point share that result.

Distinct assigned results use separate expression nodes. This preserves their
independent dependencies and incremental evaluation. Shared operations inside
distinct expressions follow the existing BG materialisation rules.

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
descriptors remain invocation-local. No BG core changes are proposed.

## Implementation and verification

1. Generalise borrowed-column binding and review its node API.
2. Add schema calculation and the graph helper using existing materialisation.
3. Verify concrete and runtime schemas, replacement, keys, nulls, source
   additions/removals, independent column updates, scalar captures, rebinding,
   and invalidation. Verify that borrowed buffers and row domains are shared.
4. Verify shared expression results are materialised once. Measure the expression
   and assembly nodes separately against the existing expression path.
