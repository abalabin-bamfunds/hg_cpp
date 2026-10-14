# Borrowed evaluation inputs

Status: superseded by [0059](0059-runtime-graph-input-context.md)

## Context

Compact reduce evaluates many instances of one compiled reducer graph. Each
instance needs two values supplied by the TSD or another instance. Persistent
runtime-input bindings retain a value pointer per input and require a separate
binding step before evaluation.

## Decision

Compiled graphs may declare borrowed input slots. The caller supplies each
slot's state, value, runtime type, and change in an `EvaluationContext` section
for one graph evaluation. The compiled binding selects the slot and uses the
normal input notification and validity paths. The runtime graph retains no
borrowed value pointer. An input cannot materialize as a reference to this
invocation-scoped storage.

Compact reduce retains per-pair input state to preserve timestamps and validity
across evaluations. It supplies current values through borrowed slots. Reducers
that require references use nested runtime graphs.

## Consequences

The caller keeps each value alive through the evaluation. A compiled reducer
graph can be shared by many runtime instances without persistent input-pointer
bindings. See [evaluation](../evaluation.md#evaluation-context) and
[reduce](../reduce.md).
