# Provider-defined queue payloads

Status: accepted

## Decision

Queue serialization and application dispatch provider-defined leaves through
`TimeSeriesQueueOperations`. Projection, writing, conflict inspection, and
application require an exact output-runtime registration. Discard is registered
by semantic type.

BG owns framing and recursive built-in collection traversal. A provider owns
its payload grammar and cleanup. Its apply callback consumes the complete
payload on both success and failure. See the
[provider queue contract](../design/time-series-providers.md#queue-messages).

## Reason

A semantic time series type can have native, borrowed, and computed runtime
representations with different valid mutations. Exact runtime registration
limits queued literals to representations that support them. Semantic discard
allows unread and partially written payloads to be cleaned without an output
binding.

Queue literals can address mutations differently from retained ticks.
Separate registrations allow both formats to use their required identities and
ownership while sharing BG's framing and traversal.
