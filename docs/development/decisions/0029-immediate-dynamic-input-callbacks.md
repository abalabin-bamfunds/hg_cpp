# Immediate dynamic input callbacks

Status: accepted

## Context

Dynamic input callbacks were queued until their receiving inner graph entered
the current outer cycle. This assumed that each inner graph had to run its own
deferred cleanup before a callback could access its runtime state.

Deferred cleanup now runs on the outer graph before any inner graph can publish
a change. Callbacks also receive the publishing evaluation time explicitly.
Waiting for the receiving graph therefore provides no ordering or lifetime
guarantee.

## Decision

Run dynamic input callbacks immediately, including callbacks whose receiving
input belongs to another inner graph.

## Consequences

Runtime graphs do not store pending input callbacks. Cross-graph callback
delivery follows the same immediate notification path as observers and static
input callbacks.

See [reference subscriptions](../design/time-series-references.md#subscriptions) and
[deferred cleanup](../design/evaluation.md#deferred-cleanup).
