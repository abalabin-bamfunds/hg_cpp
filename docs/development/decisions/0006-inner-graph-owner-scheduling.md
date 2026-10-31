# Inner graph owner scheduling

Status: superseded by [Shared runtime graph evaluator](0030-shared-runtime-graph-evaluator.md)

## Context

A node can own many inner runtime graphs. Scheduling the owning node without
identifying the inner graph would require the owner to inspect every instance.

## Decision

An inner runtime graph can carry an owner-supplied instance token and shared
callbacks. It reports the token when it receives immediate work and when its
earliest scheduled time changes. The owner tracks ready instances and scheduled
times by token and mirrors only its earliest time to the outer graph.

The existing single-inner-graph connection remains a direct representation.
The runtime stores direct and callback connections in the same union.

## Consequences

Scheduling work is proportional to changed or ready instances. One callback
record is shared by all instances of an owner type, and `RuntimeGraph` does not
grow for the callback form.

See [nested graphs](../design/nested-graphs.md#evaluation).
