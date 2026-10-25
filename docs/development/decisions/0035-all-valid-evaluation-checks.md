# All-valid checks at evaluation attempts

Status: accepted

## Context

An `all_valid` collection input can contain invalid items while its root remains
valid. Scanning the collection for every item notification makes passive inputs
perform work even when no evaluation is requested.

## Decision

Check immediate items before each scheduled evaluation attempt. Do not scan them
only because an item changed. Required-input invalidation and revalidation
notifications still refresh the complete input validity conjunction and apply
invalidation behavior when needed.

Keep declared-type and runtime-type descriptors for `all_valid` inputs in the
compiled node. Place them after the runtime-type pointer array and encode that
array's count in the compiled node flags so the descriptor location needs no
extra metadata when the array is empty.

## Consequences

Passive inputs defer contained-item checks until an event schedules their node.
Timers and other inputs can therefore recheck a passive collection. Nodes with
`all_valid` inputs carry descriptors; nodes without them carry no all-valid
descriptor data.

See [evaluation](../design/evaluation.md#required-input-invalidation) and
[validity](../design/time-series-validity.md#nodes).
