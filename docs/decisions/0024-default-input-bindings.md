# Default input bindings

Status: accepted

## Context

An invalid value input can require a scalar fallback without adding a graph
node for every ordinary consumer.

## Decision

Wire `default_` as an adapter node with source and const fallback inputs. Its
node template identifies those inputs for compiler lowering. Compilation lowers
each consumer input to a direct input binding with one runtime timestamp.
A reference consumer or nested graph boundary uses the normal reference node
over the adapter output. The result has an ordinary guarantee.

## Consequences

Fallback selection adds no runtime node for lowered value consumers. The
timestamp records source notifications and the visible input update time. A
source validity switch publishes an update even when its scalar value equals
the fallback.

See [operators](../operators.md) and [time series validity](../time-series-validity.md).
