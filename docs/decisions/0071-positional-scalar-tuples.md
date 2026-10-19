# Positional scalar tuples

Status: accepted

## Decision

`Tuple<T...>` is a distinct scalar type with structural identity from its
ordered item types. Python fixed-length tuple annotations declare this type;
homogeneous `tuple[T, ...]` annotations continue to declare scalar lists.
See the [type system](../type-system.md) and [scalar API](../scalar-api.md).

Tuple construction reuses indexed scalar field callbacks and the scalar
allocation pool. Tuple runtime types own their positional layout.

## Reason

Fixed-length tuples require positional compatibility and typed item access.
The shared `Schema` describes named fields, declaration identity, inheritance,
and optional presence. Keeping tuple semantics separate preserves those
contracts while reusing scalar storage and construction operations.
