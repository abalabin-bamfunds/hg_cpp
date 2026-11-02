# Shared hierarchical scalar storage

Status: accepted

## Decision

A hierarchical scalar value stores one pointer-sized shared handle. The handle
refers to a control block that contains an atomic reference count, its actual
hierarchical runtime type, optional-field presence, and inline field storage.
`Scalar` and `ScalarValue` remain two words and do not allocate an outer owning
object.

Each outer `GlobalState` owns one thread-safe pool through an intrusive
allocation owner. Hierarchical control blocks retain the allocation owner.
An owning hierarchical value can therefore outlive the runtime graph and
global state that created it. The final handle release destroys present fields,
returns the control block to the pool, and releases the allocation owner.
The upstream memory resource must remain alive until the allocation owner is
released. Construction outside an allocation context therefore requires an
explicit allocator.

`HierarchicalScalarRuntimeType` is the runtime interface. The engine's composed
implementation owns the standard field layout. Other runtime types for the
same semantic type can provide another physical layout through the same field
access interface. `bundle<Schema>()` constructs only the composed representation,
so allocation-owner handling remains private to that implementation.

A destination scalar runtime type validates representation compatibility before
construction or assignment. The composed hierarchical runtime accepts a
compatible derived hierarchical value only when both use its shared handle
layout. Another hierarchical runtime layout must provide its own conversion.
List and dictionary runtime types implement compatible covariance by rebuilding
destination storage item by item. Differently declared collection storage is
not interchangeable. See the
[scalar API](../design/scalar-api.md#hierarchical-scalars).

## Context

Hierarchical values require optional fields, derived values viewed through a
base schema, and recursive schemas. Copying every field into a separate outer
`ScalarValue` allocation would increase value size and make cross-graph
lifetime management depend on the graph allocator.
