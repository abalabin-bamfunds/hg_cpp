# Boolean column operators use nullable logic

## Decision

Boolean column expressions support `&&`, `||`, and `!` with nullable
three-valued logic. A known false operand determines `&&`; a known true operand
determines `||`; all other cases with a null operand produce null. Negating null
produces null. Captured `TS<bool>` values remain ordinary BG dependencies, so
their invalidation invalidates the expression output.

See the [expression API](../../README.md#column-comparisons).

## Reason

Frame Boolean cells are nullable, so logical operators must preserve unknown
values. BG owns dependency validity and scheduling; the frame extension applies
nullable Boolean logic in the block callback without adding separate graph
short-circuit behavior.
