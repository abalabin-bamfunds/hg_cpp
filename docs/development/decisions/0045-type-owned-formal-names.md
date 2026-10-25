# Type-owned formal names

Status: accepted

## Context

Diagnostics need stable names for built-in and extension types, including types
nested inside other types.

## Decision

`Type::format_name(result)` appends the formal diagnostic name. Each semantic
type formats its own structure and delegates nested types through the same
operation. `Type` and `ScalarType` provide the `type` and `scalar` fallback
names.

## Consequences

Diagnostic output uses the type's virtual interface. Extensions can define names
for their semantic types and compose them inside built-in types.
