# Scalar expressions and graph bindings

## Decision

`Expression` is an immutable scalar. It records operations, named columns and
parameters, owned scalar values, and scalar conversion targets. It can be
constructed before a graph exists.

An operation involving a wiring point uses BG operator dispatch. Its ordinary
TSB result holds a constant scalar expression and explicit time series
bindings. Internal binding references are distinct from user names.

Binding resolves columns and parameters and calls BG operators to select typed
block functions. Concrete materialisation compiles those functions through the
existing column expression contract. Parameter updates supply new values to
that computation without rebuilding the scalar expression.

## Reason

The scalar expression must be reusable across frames and graphs. BG must own
time series dependencies and their lifetimes. Keeping those dependencies in
ordinary TSBs allows mixed operations to use existing graph wiring and validity
rules.

See [expression construction and binding](../../README.md#expression-construction-and-binding)
and the [block-operation contract](../../block_operation_proposal.md).
