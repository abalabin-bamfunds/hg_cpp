# Bundles with schema variables

Status: accepted; registration transactions superseded by
[single-threaded type registration](0094-single-threaded-type-registration.md).

## Decision

`Bundle<SCHEMA>` captures the supplied semantic schema during wiring. Resolving
that schema returns the canonical resolved bundle type. A schema association
links C++ registration and runtime-schema construction to the same semantic
object and joins the existing recursive registration transaction.

The hierarchical scalar family separates unresolved schema requirements from
resolved field metadata. Runtime types reference resolved semantic types.

A descriptor with C++ declaration identity requires prior bundle registration.
This is required to retain the canonical C++ type when the runtime factory
cannot recover its template from the descriptor. Normal C++ type access performs
that registration. Runtime-defined and anonymous schemas construct directly.

## Consequences

Wiring captures the supplied semantic type; scalar views inspect the actual
runtime object. A base annotation therefore exposes derived fields without
changing schema capture.

Generic owning bundles use the actual runtime type. Composed values retain
their existing shared handle. The [storage decision](0019-shared-hierarchical-scalar-storage.md) continues to
define allocation and lifetime. Runtime-schema construction uses the same
composed runtime as C++ bundle construction.

The [type system](../design/type-system.md#hierarchical-scalars) and
[scalar API](../design/scalar-api.md#hierarchical-scalars) define the interfaces.
