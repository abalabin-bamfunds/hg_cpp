# Runtime-selected tick replay

Status: accepted

## Context

Generic TSD items inherit their static output specification from the containing
TSD. That specification does not identify an item's runtime representation.

## Decision

Register one replay handler per semantic type or runtime representation. An
exact runtime registration selects the provider's typed output view. A semantic
fallback must support every runtime representation without an exact handler.

## Consequences

Replay dispatch uses the output item's runtime type and preserves its output
state, including its containing collection path.

See [time-series types](../design/time-series-types.md#retaining-ticks).
