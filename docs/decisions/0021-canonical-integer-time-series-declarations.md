# Canonical integer time series declarations

## Decision

Concrete time series declarations use `i64` for canonical integer values.
`TS<int>`, integer collection declarations containing `int`, `TSS<int>`, and
`TSD<int, ...>` are unsupported. Raw C++ integer arguments still canonicalize
to `i64` at construction and wiring boundaries.

## Context

An `In<TS<int>>` view promises access to an `int`, so resolving that declaration
to `TS<i64>` would make its C++ interface disagree with its runtime storage.
Rejecting the noncanonical declaration keeps the view type and runtime scalar
type consistent while preserving convenient integer arguments in graph code.

See the [type system](../type-system.md#scalar-types).
