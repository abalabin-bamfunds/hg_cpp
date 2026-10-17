# Global evaluation context

Status: accepted

## Context

Child evaluation contexts copied service pointers that do not vary by graph or
node. Timer operations need access to the outer evaluator independent of graph
nesting depth.

## Decision

Separate shared services into `GlobalEvaluationContext`. Child contexts borrow
the same object and access its outer graph and evaluator references directly.
The engine owns this object for the run; direct runtime graph entry calls own
one for the call. This requires no persistent fields on runtime graphs.

Keep evaluation time local to provide direct access during node execution.
The [context benchmark](../../benchmarks/evaluation_context.md) records the
measurements used to select its placement.

Scoped bindings, borrowed graph inputs, and scratch frames remain local because
their lifetime and values depend on the invocation.

See [evaluation context](../evaluation.md#evaluation-context).
