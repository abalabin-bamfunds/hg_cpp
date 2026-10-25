# Runtime graph input context

Status: accepted

## Context

Compact reduce evaluates many instances of one compiled reducer graph. Each
instance supplies two invocation-scoped time-series values. The compiled graph
already identifies the graph and each slot, so keyed evaluation-context
sections add no required routing information.

## Decision

The caller supplies a pointer to `RuntimeGraphInput` records to the target
`RuntimeGraph` entry call. The compiled graph defines the record count. Graph
entry creates a graph-local evaluation context with that pointer; evaluation
contexts do not inherit runtime graph inputs. Ordinary nested graphs pass
`nullptr` unless they have their own records. The caller supplies at least the
compiled slot count of records with types and per-call fields that satisfy
their compiled bindings. The runtime uses the compiled slot count to iterate
them and does not validate or retain the pointer beyond the call.

See [evaluation](../design/evaluation.md#evaluation-context) and [reduce](../design/reduce.md).
