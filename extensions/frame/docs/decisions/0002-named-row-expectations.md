# Named row expectations

Status: accepted

## Context

Unkeyed frames and columns expose internal row IDs to nodes. Test literals
must identify additions, modifications, and removals without choosing those
IDs. Different rows can have identical values when added.

## Decision

Expected deltas assign comparison-local names to row lifetimes. Comparison
records actual and expected row histories across cycles, then matches equal
complete histories one to one. This is required to distinguish identical
additions by their later changes. Identical complete histories are
interchangeable, so matching does not require permutation search.

A name remains reserved after removal or invalidation. Recycling an internal
row ID starts a new actual history. Each `equals()` call and each nested time
series have independent comparison state.

## Consequences

Production row identity and input literal syntax remain independent of test
names. Native capture stores row IDs and changed values; comparison owns the
histories needed by the assertion. Keyed frames continue to match expected
deltas by user key. See [named row expectations](../../README.md#named-row-expectations)
for the public syntax.
