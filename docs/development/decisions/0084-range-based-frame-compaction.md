# Frame compaction by contiguous runs

Status: accepted

Supersedes [frame compaction mechanics](0083-frame-compaction-mechanics.md).

## Context

Per-cell preparation and cleanup dispatch dominate compaction of primitive frame
columns. Each planned live run already lies within common mapping and column
buffer boundaries, so its cells can move without allocating numeric buffers.

## Decision

Keep storage-owned planning, relative row order, the longest live run as anchor,
and revision-checked cached physical mappings. Prepare a complete contiguous
live run across every owned column before moving values. Preparation grows
validity capacity and initializes required packed Boolean bytes.

Relocate numeric valid subruns, packed Boolean bits, and validity without
allocation. Then update both mapping directions and live occupancy without
throwing. Mapping entries remain constructed throughout movement.

See the [compaction proposal](../../extensions/frame/compaction_proposal.md)
for the full operation and failure contract.

## Consequences

Preparation failure leaves the current run's logical values and mapping intact;
prepared capacity can remain allocated. Earlier completed runs remain committed.
Compaction preserves IDs, keys, row order, values, and deltas. The operation
remains explicit; automatic scheduling and a compaction policy remain undecided.
