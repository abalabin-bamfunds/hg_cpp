# Incremental column sum state

Status: accepted

## Context

TSC deltas identify changed and removed rows. They do not retain previous cell
values, and removed storage can be reused before consumers evaluate.

## Decision

Implement numeric column sum as an ordinary BG node registered on
`operators::sum_`. Each node owns previous non-null contributions by row ID and
updates its aggregate from row deltas. This is required to subtract previous
contributions without retaining source cells.

Rebuild state from current batches when the source binding changes, the output
revalidates, or the current delta is unavailable. Filter membership changes use
the same addition and removal operations.

Count NaN and each infinity sign separately from the finite subtotal. This is
required to recover when a non-finite contribution is removed. If finite
arithmetic overflows, rescan cached contributions after the next row change.

See [Column reductions](../../extensions/frame/README.md#column-reductions)
for result semantics.
