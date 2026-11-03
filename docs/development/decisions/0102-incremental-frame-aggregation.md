# Incremental frame aggregation

Status: accepted

## Context

Frame deltas identify changed and removed rows without retaining previous cells.
Grouping must retract old contributions and avoid reading unchanged grouping
columns on ordinary value updates.

## Decision

The aggregation node retains row-to-group membership. Each aggregation kernel
retains its row contributions. This is required to process value updates and
removals from row IDs while preserving group output rows across evaluations.

Expressions contribute aggregation kernels through BG's scalar dispatch and
existing wiring materialisation. The node uses the shared column expression
compiler to evaluate operand blocks. This is required to compose aggregation
with column expressions without materialising each intermediate column.

Keyed reference sampling supplies simultaneous row-ID remappings. Consumers
remove cached contributions for removed and remapped old IDs before processing
the current rows. Source key matching belongs to the sampled input.

See the [aggregation design](../design/frame-group-by.md) for the kernel
interface and [grouped sums](../../user/extensions/frame-expressions.md#grouped-sums)
for result semantics.
