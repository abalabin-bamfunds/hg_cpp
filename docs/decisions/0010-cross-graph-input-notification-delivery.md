# Cross-graph input notification delivery

Status: accepted

## Context

A dynamic input callback can originate in an inner graph and update an input in
an outer graph during the same outer cycle. Deferring that callback until the
outer graph's next cycle exposes stale binding state. A graph that has not yet
entered the current cycle must still run deferred cleanup before a callback can
access its runtime objects.

## Decision

Apply a dynamic input callback immediately when the receiving graph has entered
the notification's outer cycle. Otherwise, queue it until that graph enters the
cycle.

## Consequences

Reference target changes from an inner graph are visible to downstream outer
nodes in the same cycle. A queued callback cannot access a receiving graph
before its deferred cleanup. Callback delivery uses existing graph timestamps
and adds no runtime state.

See [reference subscriptions](../time-series-references.md#subscriptions) and
[outer-cycle evaluation](../evaluation.md#outer-cycle).
