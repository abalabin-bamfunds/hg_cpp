# Scalar extension dispatch

Status: accepted

## Context

Independent libraries must be able to add scalar types and optional operations
for types owned by another library. Adding an operation such as Python
conversion to `ScalarType` or `ScalarRuntimeType` would couple every scalar
provider to that integration.

## Decision

Scalar providers export stable semantic and canonical runtime type identities.
Optional integrations own `ScalarDispatch` registries keyed by exact runtime
type or semantic type. Runtime-type handlers take precedence. Recursive
operations use `Scalar::visit()` for structural collections and consult their
registry only from the `Scalar<>` catch-all.

## Consequences

Adding an optional operation does not change scalar values or type interfaces.
An integration can optimize one physical representation without changing the
semantic fallback.

See [scalar API](../design/scalar-api.md).
