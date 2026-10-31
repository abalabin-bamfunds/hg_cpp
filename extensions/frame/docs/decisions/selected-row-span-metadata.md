# Selected row spans

Status: accepted

## Context

[Traversal profiles](../../benchmarks/results/2026-10-22/profiling.md) showed
that consumers reconstructed contiguous batches through one physical-slot
lookup per selected row. Filter membership changes also discarded and rebuilt
the complete selected-ID storage.

## Decision

The [row traversal contract](../../README.md#values-and-rows) defines ordered,
callback-scoped ID spans and their optional physical mapping. A column uses
consecutive slot offsets only when it shares that mapping.

A filter retains an initialized prefix of row-ID storage and overwrites its
active prefix during rebuilds. Each ID storage block has one mapping record.
The record supplies consecutive slots only when every active ID in the block
has that mapping and slot order.

## Consequences

Consumers can traverse dense selections by storage block. Mixed selections
retain ID lookup and existing batch coalescing. Computed columns with a
different mapping continue to resolve IDs through their own storage.

Mapping metadata grows with the number of row-ID storage blocks. Storage
blocks grow geometrically up to the bunch utility's 256 KiB block limit.
Selection rebuilds reuse retained capacity; storage release destroys the
initialized prefix. Capacity trimming remains separate work.
