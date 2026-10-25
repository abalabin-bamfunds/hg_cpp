# Python homogeneous tuple annotations

Status: superseded by [Positional scalar tuples](0073-positional-scalar-tuples.md)

## Decision

Python `tuple[T, ...]` and `typing.Tuple[T, ...]` normalize to the existing
`bg::List<T>` scalar type. Scalar list values materialize as Python tuples.
Mutable list annotations and fixed tuple annotations are unsupported. See the
[Python scalar conversion contract](../design/python.md).

## Reason

The homogeneous tuple annotation expresses read-only access and variable length,
matching the existing scalar list interface. Normalizing at the Python boundary
preserves engine type identity, generic matching, and storage operations.
