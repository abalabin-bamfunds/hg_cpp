# Native compound scalars

Status: accepted

## Context

Native fixed-layout records need typed C++ access and structural access for
generic operations. Their equality and hashing must not depend on padding.
Libraries must observe one semantic identity for the same C++ record type.

## Decision

A valid native aggregate maps to a nominal compound scalar that the engine
canonicalizes process-wide. Canonicalization validates its reflected name,
native layout, and ordered field schema. Each module can instantiate a native
runtime type for that semantic identity. The runtime type stores the aggregate
in its native layout and provides allocation-free structural access.

Compound scalar equality and hashing process fields. `Scalar::visit()` selects
`Scalar<STRUCT>` as a structural alternative.

## Consequences

Concrete code reads the native aggregate directly. Generic code traverses the
same value without padding-based comparison or offset arithmetic.
Canonicalization detects inconsistent declarations across libraries.

See [scalar API](../design/scalar-api.md) and [type system](../design/type-system.md).
