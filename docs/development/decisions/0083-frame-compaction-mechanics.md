# Frame compaction mechanics

Status: superseded by [frame compaction by contiguous runs](0084-range-based-frame-compaction.md).

## Context

Deleted rows leave gaps in frame-owned physical storage. Moving live cells can
reduce batch and Arrow chunk counts, but compaction must preserve logical row
identity, order, values, and change information. Cached row-to-slot proofs can
become stale after movement.

## Decision

Keep compaction owned by the storage that owns the physical row mapping. Within
each compatible storage range, preserve the live rows' relative order and use
the longest existing live run as the anchor. This minimizes moved rows when
relocation cost is equal. Prepare each row across its owned columns before
committing the row mapping; roll back an incomplete row preparation.

Advance the mapping layout revision before the first committed move. Cached
selected-order proofs are valid only for the revision where they were
established; revalidate stale proofs before using them.

See the [compaction proposal](../../extensions/frame/compaction_proposal.md)
for the storage cases, failure behavior, and verification coverage. Automatic
compaction policy is not selected by this decision.

## Consequences

Compaction does not change row IDs, visible order, values, or deltas. A failure
can leave earlier complete row moves committed, while the failed row remains at
its source slot. Subsequent readers detect stale cached proofs through the
revision. The compaction operation is explicit; this decision does not select
an automatic trigger or threshold.
