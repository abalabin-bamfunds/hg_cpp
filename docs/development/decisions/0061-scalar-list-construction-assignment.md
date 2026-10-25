# Scalar list replacement through construction callbacks

Status: accepted

## Decision

Scalar list runtimes replace a stored value through a synchronous
`ScalarConstruction` callback using the active scalar allocator. Native and
composed lists construct one replacement and swap their storage headers.
Provider runtimes can use the default assignment fallback or override it for
their representation. See the [scalar construction contract](../design/scalar-api.md#ownership).

`Out` owns equality suppression and publication. The literal consumer owns
source syntax and recursive visitor dispatch. Runtime type APIs receive only
source-neutral construction callbacks.

## Reason

Runtime-owned replacement preserves each provider's physical layout and
allocation context. Built-in storage swaps avoid a second collection copy.
Borrowed literals can reuse construction without exposing their representation
to runtime types.
