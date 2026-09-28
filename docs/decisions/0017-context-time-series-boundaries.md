# Context time series across graph boundaries

Status: accepted

## Context

An inner graph can request a time series from an enclosing wiring scope. The
same compiled graph must remain reusable when another instance obtains that
context from a different wired output. Inner graph owners must not forward
later changes manually.

## Decision

Wiring records each inherited context by name and semantic type. The first
graph boundary creates a stable reference in the provider graph. Each inner
graph owner receives that reference through a passive input and places it in a
child evaluation context when the inner runtime graph starts.

The compiled inner graph stores a reference slot for each requirement.
Consumers use the normal reference input and subscription machinery. Graph
structure identity includes the context name and type but excludes the
provider identity.

## Consequences

Compiled graph structure can be reused across context providers. Context-free
graphs allocate no context slots. Each requirement adds one `EvaluationTime`,
one `Reference`, and its compiled notification metadata. Later changes follow
normal reference notification paths.

See [graph wiring](../graph-wiring.md#context-time-series) and
[nested graphs](../nested-graphs.md#graph-boundary).
