# Runtime input bindings

Status: accepted

## Context

External time series need to enter a compiled graph without adding a producer
node or an owning reference for each input. Wiring knows the input's runtime
representation, so resolving it again on each update would add avoidable runtime
work.

## Decision

Represent each external input with a 16-byte runtime slot containing its
sampled `TimeSeriesState` and a borrowed value pointer. Wiring validates and
records the runtime representation. Compilation uses that metadata to build
the normal subscriber and input-notification paths; binding and clearing
publish changes through those compiled paths rather than through a synthetic
node or reference.

Support ordinary `TS<V>` and `REF<TS<V>>` inputs. Do not create an owning
reference to materialize an external value input.

## Consequences

The caller keeps a bound value alive until it binds or clears that input again.
It must bind or clear between graph cycles, using an evaluation time later
than the graph's previous cycle. Collections, final guarantees, and
value-to-owning-reference materialization remain outside this interface.

See [evaluation](../evaluation.md#evaluation-model) for binding behavior and
lifetime requirements.
