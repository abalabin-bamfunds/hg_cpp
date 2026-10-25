# Contextual operator specificity

Status: accepted

## Context

Plain node parameters accept scalars or time series. Their union declarations
can overlap explicit time series declarations and constrained type variables.
Overload selection must compare these declarations consistently while retaining
relationships created by repeated variables.

## Decision

Compare applicable signatures recursively using a partial order. Use each
candidate's supplied types to identify applicable union alternatives, then
compare their original declarations. Preserve union variable identities through
nested comparisons. Combine type restrictions and repeated-variable
relationships without numeric specificity scores.

## Consequences

Conflicting restrictions remain ambiguous. Comparison without supplied types
remains conservative. The comparison runs during wiring and adds no graph
runtime state.

See [operator specificity](../design/operators.md#specificity) and
[type variables](../design/type-system.md#variables).
