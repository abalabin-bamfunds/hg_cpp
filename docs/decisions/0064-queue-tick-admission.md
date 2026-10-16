# Queue readers inspect before replay

Status: superseded by [0065](0065-consuming-queue-application.md)

## Decision

Standard queue readers inspect a reader fork before replaying an accepted
payload. Inspection uses output timestamps, validity, and scalar equality to
detect the first conflicting effective change. See
[queue tick application](../time-series-types.md#queue-messages).

## Reason

A conflict must leave the complete message pending, including changes that
precede the conflict in the payload. A reader fork preserves the original
cursor and ownership while inspection checks the output without mutation.
Non-destructive scalar decoding supports both inspection and replay; discard
owns payload destruction. The visitor owns decoding because the buffer grammar
belongs to the queue implementation.
