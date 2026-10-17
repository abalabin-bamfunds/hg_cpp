# Batch queue messages carry leaf elements

Status: accepted

## Decision

Batch mode interprets each `TS<List<T>>` leaf payload as one `T`. The target's
mode selects this encoding within the existing tick grammar. Replay constructs
or appends the item directly through the output's scalar list runtime. See
[queue messages](../time-series-types.md#queue-messages).

Batch admission inspects a reader fork before consuming replay. Conflicting
invalidation and removal messages remain pending in their entirety.

## Reason

Direct accumulation uses the output list's layout and allocator and transfers
owning scalar values without constructing a temporary owning list per message.
Deferring conflicting invalidation and removal preserves values accumulated
from earlier messages until cycle publication. Complete-message inspection
preserves queue order and encoded payload ownership.
