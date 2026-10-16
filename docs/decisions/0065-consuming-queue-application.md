# Queue application consumes accepted payloads

Status: accepted

## Decision

Standard queue admission inspects a reader fork without changing the output or
encoded objects. Accepted replay consumes the original reader and transfers
owning scalar values into outputs. Custom scalar encodings use their existing
read and discard handlers locally. See
[queue tick application](../time-series-types.md#queue-messages).

## Reason

Inspection must preserve a conflicting message for a later cycle. After
acceptance, consuming replay avoids copying owning scalar values and traversing
the complete message again for destruction. Replay retains enough traversal
state to destroy unread values after application fails, without rolling
back earlier mutations.

This replaces the borrowed replay and complete-payload discard in
[0064](0064-queue-tick-admission.md).
