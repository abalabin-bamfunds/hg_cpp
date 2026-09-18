# Variadic wiring records

Status: accepted

## Context

Variadic arguments must remain inspectable during wiring, materialize through
ordinary time series bundle inputs, and cross nested graph boundaries without
adding a third input category.

## Decision

Represent a captured pack as a wiring record with a time series bundle (TSB)
projection. Own scalars in graph wiring and materialize them lazily through
`const_`. Expand captured time series to nested boundary references while
retaining scalar captures as inner compiled data.

## Consequences

The pack remains a normal wiring point and ordinary TSB inputs need no special
case. Unused packs create no constant or TSB nodes. Nested scalar captures do
not become boundary inputs.

See [variadic wiring](../variadic-wiring.md).
