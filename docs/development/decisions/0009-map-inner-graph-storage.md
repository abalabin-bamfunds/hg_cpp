# Map inner graph storage

Status: accepted

## Context

Map reconciles all key changes before it starts or stops inner graphs. A stopped
inner graph remains valid through that outer cycle and is destroyed before new
input updates in the next cycle. A replacement graph for the same key therefore
cannot coexist with it.

High-count maps also require compact ready-instance tracking. Measurements from
50,000 to 1,000,000 entries and 1% to 10% random activation showed competitive
runtime cost from a flat bitmap throughout that range.

## Decision

Each map entry owns its `NestedGraph`. Its stable dictionary position is the
inner graph owner token. A flat bitmap records ready positions. A dense vector
records positions whose graphs stopped during the current cycle.

The next-cycle callback erases each stopped entry after its `NestedGraph` has
destroyed the stopped runtime graph.

## Consequences

One entry and one inner graph exist for each map key. The runtime requires no
separate inner-graph allocation, cross-pointers, intrusive ready links, or
stopped-graph count.

Ready tracking uses one bit per allocated dictionary position and checks one
word per 64 positions. Stopped-entry processing remains proportional to the
number stopped.

See [map lifecycle](../design/map.md#evaluation-and-lifecycle) and
[deferred cleanup](../design/evaluation.md#deferred-cleanup).
