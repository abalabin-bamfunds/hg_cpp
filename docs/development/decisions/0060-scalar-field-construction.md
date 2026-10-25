# Scalar field construction callbacks

Status: accepted

## Decision

Scalar runtimes construct fields through a source-neutral callback indexed by
schema position. The runtime owns layout, optional presence, publication, and
cleanup. See the [scalar construction contract](../design/scalar-api.md#ownership).

Native compounds construct their trivial fields in fixed stack storage and
initialize the aggregate once. Composed hierarchical scalars construct fields
in final storage and share the allocation owner during nested construction.

## Reason

Initializing a native aggregate once supports const fields through standard
C++ object construction. Direct hierarchical field construction avoids
intermediate owning values. Sharing the allocation owner preserves a nested
value's storage when an owning copy outlives its parent.
