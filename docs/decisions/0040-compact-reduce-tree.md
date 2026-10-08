# Compact reduce tree

Status: accepted

## Context

Nested reducer graphs require runtime graph storage for each pair of TSD items.
That storage dominates simple reducers, and rebuilding every pair after a
sparse item update repeats unchanged work.

## Decision

For reducer graphs that support borrowed evaluation with compatible runtime
representations, reduce stores valid items in a compressed binary Patricia tree
keyed by physical TSD item IDs. Internal branches own borrowed reducer storage.
TSD deltas update tree leaves and reevaluate affected paths. Other reducer
graphs continue to use nested runtime graphs.

The reducer operation must be associative and commutative because physical item
IDs can change the tree's grouping and order.

## Consequences

Sparse item changes reuse unaffected reducer storage and evaluation results.
Membership changes and ID remapping update the tree structure. The fallback
path retains nested graph lifecycle semantics.

See [reduce](../reduce.md).
