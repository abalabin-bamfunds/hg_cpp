# Type-specific variadic collections

Status: accepted

## Context

Variadic parameters can project arguments as different collection types. A
prebuilt collection also needs to retain its identity instead of becoming one
captured item or expanding into item references.

## Decision

Each collection type defines variadic capture, projection, item inspection, and
materialization. A prebuilt collection supplied with the exact variadic
parameter name binds directly. It crosses a nested graph boundary as one
reference.

Resolvers and requirements inspect collection items through type-only argument
descriptors. This does not select or materialize the items.

## Consequences

TSB supports scalar and time-series capture. TSL supports homogeneous
time-series capture with wiring-time size resolution. Direct collection binding
preserves lazy item selection and avoids per-item boundary references.

See [variadic wiring](../variadic-wiring.md).
