# Structural graph arguments

Status: superseded by [registered types and graph constraints](0037-registered-types-and-graph-constraints.md)

## Context

Higher-order operators must distinguish graphs by behavior rather than only by
signature. Overload resolution and caching must not depend on one graph object
or lambda instance.

## Decision

Represent a graph wiring argument with an exact structural type and an owned
opaque payload. Treat its ordered input boundaries as external vertices, so
their producer stub nodes do not affect identity. Match a graph pattern by
wiring it against the supplied boundary types in an isolated graph and
comparing the resulting structure.

Represent a declared graph signature as a graph type. Match its parameter and
result types through the caller's type map. This lets type variables relate a
graph signature to the other parameters of a higher-order operation. Apply an
optional graph pattern as an additional structural constraint.

Use graph structure in operator cache keys. Bypass caching when the structure
contains a value that cannot be compared and hashed.

## Consequences

Equivalent graph objects share overload-resolution results. Node definitions,
connections, output presence, and stored scalar values remain semantic. Graph
arguments remain wiring-only and cannot enter node runtime storage.

See [graph wiring](../design/graph-wiring.md#graph-templates) and
[operators](../design/operators.md#graph-arguments).
