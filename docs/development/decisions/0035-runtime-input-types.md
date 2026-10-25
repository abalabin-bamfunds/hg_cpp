# Runtime input types

Status: superseded by [borrowed evaluation inputs](0058-borrowed-evaluation-inputs.md)

## Context

Runtime input slots provide a time-series state and borrowed value pointer.
Reference inputs already have a compiled binding path that resolves the target
and handles its notifications.

## Decision

Runtime input endpoints accept ordinary `TS<V>` values only. References use
the normal compiled reference binding path. This keeps runtime input binding
direct and avoids a second reference binding mode in the compact evaluator.

## Consequences

Runtime input slots do not represent `REF<TS<V>>`. See
[evaluation](../design/evaluation.md#evaluation-model) for binding behavior and
lifetime requirements.
