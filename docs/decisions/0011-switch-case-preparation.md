# Switch case preparation

Status: accepted

## Context

A switch case set can contain heterogeneous graph templates and graph
functions. Operator resolution and its cache must distinguish the complete
ordered case set. Each case graph must retain independent compiled data while
only the selected case requires runtime state.

## Decision

`switch_` is an operator. Its call preparation wires every case separately and
passes their ordered graph structures as one graph argument constraint. The
default overload compiles every case once. Its runtime node owns one
`NestedGraph` for the selected case and publishes a reference to that graph's
output.

## Consequences

Normal operator overloading can select another implementation from the complete
case set and supplied arguments. Equal key updates retain the active graph.
Changing the selected case replaces it with fresh runtime state while normal
nested-graph cleanup keeps the stopped graph alive through the current cycle.

See [switch](../switch.md), [structural graph arguments](0007-structural-graph-arguments.md),
and [nested graphs](../nested-graphs.md).
