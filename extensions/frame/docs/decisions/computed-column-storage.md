# Computed columns retain source row identity

## Decision

Computed `TSC` and `TSF` outputs retain their source's logical row IDs, row
domain, and traversal order. They own result cells in local physical slots. A
computed `TSC` owns one slot map; computed columns in one frame share a slot
map. Borrowed columns retain their source buffers.

The frame extension selects this layout through its output declaration and BG's
output runtime type hook. The semantic frame type continues to describe the
schema. See the [output API](../../README.md#mixed-computed-and-borrowed-columns).

## Reason

Streaming column operators align inputs and propagate changes using source row
identity. Independent physical slots let result storage reuse free slots while
preserving that identity. Sharing one map across computed frame columns avoids
storing a row mapping for each result column. A computed `TSC` allocates slots
only for its own result cells.

Computed storage applies each accepted membership transition directly. This is
required to release cells even when public row deltas cancel an addition and
removal in the same cycle.
