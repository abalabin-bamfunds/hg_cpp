# Map compilation and overloading

Status: accepted

## Context

Mapping a graph over many keys must avoid compiling the same graph for every
key. Alternative implementations must remain selectable through normal operator
overloading.

## Decision

`map` is an operator. Its call builds one structural graph argument for overload
resolution. The default overload compiles that graph once and creates one inner
runtime graph for each current key.

## Consequences

Overloads can select a different implementation from the mapped graph structure
and supplied arguments. The default implementation shares compiled data while
each key retains independent runtime state and scheduling.

See [map](../design/map.md), [graph arguments](0007-structural-graph-arguments.md), and
[inner graph owner scheduling](0006-inner-graph-owner-scheduling.md).
