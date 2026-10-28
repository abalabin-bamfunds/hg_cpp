# Graph output inference from the returned wiring point

Status: accepted

## Context

A graph can calculate its output schema during its body invocation. Input
matching cannot resolve output-only variables in that schema.

## Decision

Use [normal graph invocation](../design/graph-wiring.md#graph-templates) to select input
forms before invoking the selected body once. Match the returned wiring point
against the original output declaration using the established input mappings
and supplied resolvers. An incompatible result fails the call.

Graph result declarations use declaration-role guarantee defaults, including
for references. An omitted guarantee therefore does not impose a node output's
deduplication default. The graph call returns the actual point after checking
it against the declaration, preserving any stronger guarantee downstream.

Operator resolution caches the selected input binding. Each graph invocation
infers its remaining output mappings from its own result.

Output matching uses the [resolver contract](../design/operators.md#resolvers) for the
current tentative type map. This is required to let a resolver depend on an
output field already matched without committing a failed match's mappings.

## Consequences

Graph composition supports schemas calculated during wiring. Output validation
preserves the declaration's relationships with input types and its resolver
constraints.
