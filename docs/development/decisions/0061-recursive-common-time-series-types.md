# Recursive common time series types

Status: accepted

## Context

Reference selection can choose between collection time series whose guarantees
differ at the root or at contained items. The selected output must retain their
shared structure without claiming a guarantee that either input can violate.

## Decision

Each time series type implements `common_type()` for its structure. It combines
guarantees at each corresponding time series, including reference targets, and
returns a canonical resolved type. Incompatible or unresolved types have no
common type. This operation runs during wiring; selection uses normal reference
outputs at runtime.

See the [common type contract](../design/type-system.md#time-series-guarantees) and
the [selection operator](../design/library.md#validity-and-tick-selection).
