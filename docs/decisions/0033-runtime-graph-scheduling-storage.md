# Runtime graph scheduling storage

Status: accepted

## Context

Each runtime graph stores evaluation requests, validity checks, and cached input
validity for its nodes. A `SchedulingInfo` block packs these three bit sets for
64 nodes. Small inner graphs can otherwise require an allocation used only for
their scheduling state.

## Decision

Keep scheduling state in packed `SchedulingInfo` blocks. Store the first block
in `RuntimeGraph`; append further blocks to the runtime state allocation after
node state. Store the initial scheduling blocks and node state together in the
compiled graph's initial image, then copy each part to its runtime location.

## Consequences

On 64-bit targets, `RuntimeGraph` grows by 24 bytes for its first scheduling
block. A graph with at most 64 nodes and no node state therefore needs no
per-instance state allocation; a graph with node state still allocates that
state. Additional scheduling blocks follow node state for graphs with more than
64 nodes. The 25k-map comparison showed no material throughput or aggregate
memory regression.

See [node layout](../node-layout.md#packed-runtime-storage) and
[evaluation](../evaluation.md#evaluation-order).
