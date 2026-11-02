# Wiring-owned operator resolution caches

Status: accepted

## Context

Resolution keys can own graph structures and scalar values. A global operator
can outlive the runtime types needed to release those values during process
shutdown.

## Decision

Root `GraphWiring` owns operator resolution caches. Nested wiring shares the
root's caches. See [resolution caching](../design/operators.md#resolution-cache).

## Consequences

Destroying the root releases cached values while their runtime types remain
available. Independent roots resolve independently. Operator lifetime no longer
determines the lifetime of cached wiring arguments.
