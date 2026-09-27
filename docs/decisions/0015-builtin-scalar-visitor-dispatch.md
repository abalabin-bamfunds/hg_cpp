# Built-in scalar visitor dispatch

Status: accepted

## Context

Generic operations need typed access to built-in atomic scalars and erased
access to types supplied by extension libraries. Adding a category field or a
visitor function to every scalar type would increase the scalar type interface.

## Decision

One compile-time declaration list defines the built-in atomic scalar set.
`Scalar::visit()` first uses the existing type visitor for structural
collections. Its scalar catch-all compares semantic type identities from the
declaration list and supplies the matching `Scalar<T>` view. A type not in the
list receives `Scalar<>`.

## Consequences

Built-in handlers use normal typed C++ overloads. Extension dispatch remains
separate and scalar type objects gain no fields or virtual functions. Built-in
selection performs a short linear identity scan during generic scalar visits;
measure this path before adding an index or registry.

See [scalar API](../scalar-api.md).
