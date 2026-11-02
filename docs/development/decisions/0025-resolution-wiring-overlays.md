# Resolution wiring overlays

Status: accepted

## Context

Variadic signature resolution needs ordinary wiring points backed by temporary
records. Registering those records with the graph retains rejected operator
candidates, while records without graph identity do not behave like ordinary
wiring records.

## Decision

Use a disposable wiring-record overlay associated with the active
`GraphWiring`. The overlay owns temporary records and gives their wiring points
the active graph identity. These records cannot materialize. A selected call
normalizes its arguments again into graph-owned records before wiring.

## Consequences

Requirements and type resolution use the normal wiring-point representation.
Rejected candidates add no records to the graph. No overlay-backed wiring point
survives the operation that owns the overlay.

See [graph wiring](../design/graph-wiring.md) and [variadic wiring](../design/variadic-wiring.md).
