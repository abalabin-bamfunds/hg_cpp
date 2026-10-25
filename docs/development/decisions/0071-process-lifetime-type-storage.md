# Process-lifetime type storage

Status: accepted

## Context

Type descriptors and canonical lookup indexes retain references to one another.
Their process-lifetime contract must include shutdown, when function-local
statics would otherwise destroy some of these objects before extension code
finishes using them.

## Decision

Committed descriptors, their registries, canonical indexes, shared operation
registries, and the scalar allocation-owner thread-local key use storage that
does not run destructors at process shutdown. Failed registration candidates,
locally constructed operation registries, allocation contexts, and graph values
retain their normal bounded lifetimes. An extension that provides a committed
descriptor remains loaded until process termination.

## Consequences

Canonical type pointers and virtual dispatch remain valid through shutdown.
This implements the lifetime contract in
[registered types](0037-registered-types-and-graph-constraints.md) and
[the type model](../design/type-system.md#type-model).
