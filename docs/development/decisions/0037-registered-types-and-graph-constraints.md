# Registered types and graph constraints

Status: accepted

## Context

Wiring results and diagnostics retain semantic type pointers after the graph
arguments or declarations that introduced them have been destroyed. Graph
structures can own callbacks and scalar values whose lifetime should remain
bounded by their use.

## Decision

Register every semantic type. Committed descriptors have process lifetime;
failed registration transactions remove their pending descriptors. Public C++
declarations manage registration automatically. Non-type variable metadata has
its own registry.

Canonical graph types describe boundary signatures. Graph arguments own their
structures and payloads; signature parameters own structural constraints.
Wiring matching checks those constraints against the actual graph argument.
Operator cache keys retain the signature type and structures separately.

## Consequences

Type-pointer identity remains valid after graph or declaration destruction.
Different graph structures can share one signature type without retaining graph
values in the process-lifetime registry.

This replaces the structural-type representation in
[structural graph arguments](0007-structural-graph-arguments.md).
See [the type model](../design/type-system.md#type-model) and
[graph templates](../design/graph-wiring.md#graph-templates).
