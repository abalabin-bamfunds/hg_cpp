# Built-in scalar type dispatch

Status: accepted

## Context

Generic scalar operations need typed access to built-in atomic declarations and
erased access to declarations supplied by extensions. Built-in identification
must not add engine-specific state or operations to the common `ScalarType`.

## Decision

Built-in atomic semantic types derive from `BuiltinScalarType`. It stores the
declaration's index from the built-in scalar list. `TypeVisitor` has one slot
for `BuiltinScalarType`, and `visit_builtin_type()` selects the declaration by
index. `Scalar::visit()` uses that selection to supply `Scalar<T>`; other
`ScalarType` instances receive `Scalar<>`.

## Consequences

Adding a built-in atomic declaration requires adding it to the declaration list
and updating the indexed dispatch. Extension types continue to derive from
`ScalarType` and use the erased scalar view.

See [scalar API](../scalar-api.md).
