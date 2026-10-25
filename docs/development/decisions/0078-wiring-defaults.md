# Wiring defaults

Status: accepted

## Context

C++ wiring signatures need defaults for omitted arguments. A default must remain
valid after its declaration object is destroyed. Resolving a candidate must
not mutate the graph or leave temporary wiring records at graph boundaries.

## Decision

Signatures own scalar default arguments. Argument normalization applies a
default only when the caller omits that parameter. Normalization represents a
scalar default for a time-series parameter with a lazy wiring record. Probe
normalization owns temporary records in an overlay; ordinary committed
normalization gives its records to `GraphWiring`. Nested graph normalization
commits default records that the outer call retains. Variadic capture records
stay in a temporary overlay and borrow supplied argument values for the
duration of nested wiring. Materialization calls `const_()`.

## Consequences

Defaults participate in type resolution and requirements through the ordinary
normalized argument path. Candidate probes can inspect defaults without
retaining probe records in the graph. Nested graph boundaries use committed
arguments before materialization.

See [graph wiring](../design/graph-wiring.md#default-arguments) and
[operators](../design/operators.md#candidate-resolution).
