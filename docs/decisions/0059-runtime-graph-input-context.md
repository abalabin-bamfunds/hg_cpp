# Runtime graph input context

Status: accepted

## Context

Compact reduce evaluates many instances of one compiled reducer graph. Each
instance supplies two invocation-scoped time-series values. The compiled graph
already identifies the graph and each slot, so keyed evaluation-context
sections add no required routing information.

## Decision

The caller supplies a pointer to `RuntimeGraphInput` records on the
`EvaluationContext` for the target runtime graph. The compiled graph defines the
record count. Same-graph child contexts inherit the pointer; contexts for a
different runtime graph clear it unless the caller supplies that graph's
records. The caller supplies at least the compiled slot count of records with
types and per-cycle fields that satisfy their compiled bindings. The runtime
uses the compiled slot count to iterate them and does not validate or retain
the pointer beyond the invocation.

See [evaluation](../evaluation.md#evaluation-context) and [reduce](../reduce.md).
