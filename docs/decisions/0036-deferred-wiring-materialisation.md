# Deferred wiring materialisation

Status: accepted

## Context

An extension can offer several representations of one wiring point. A consumer
can require a concrete representation or retain the available choices for a
later graph operation.

## Decision

Select an overload during graph construction. Its output keeps one semantic
type and advertises typed materialisation options with costs and declared input
signatures. Graph-only unions retain upstream choices; concrete parameters
record materialisation requirements.

Complete declared requirements before executing materialisation callbacks.
Cache each selected expansion and keep it fixed when generated calls introduce
further requirements or services. Complete all pending calls and services before
closing wiring and compiling the graph.

Validate declared contracts at the originating call. Deferred failures retain
the original path and requesting consumer, parameter, or service context.

## Consequences

Extensions can defer representation selection without introducing another
runtime execution engine. Callback signatures expose dependencies through the
ordinary wiring matcher. Compilation consumes completed wired outputs.

See [graph wiring](../graph-wiring.md#deferred-wiring-points) and
[operator resolution](../operators.md#candidate-resolution).
