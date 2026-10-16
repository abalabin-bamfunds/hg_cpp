# Queue closure waits for accepted writes

Status: accepted

## Decision

Queue handles share a closed flag and an active-write count. Closure rejects new
writes before serialization and waits for accepted writes to finish. The reader
then destroys unread typed payloads. See [queue handles](../time-series-types.md#queue-messages).

## Reason

Retained handles can outlive the graph. Rejecting writes before serialization
prevents them from accessing destroyed runtime types. Waiting for accepted
writes lets the reader finish typed cleanup after every reservation is committed.
Payload allocation uses a process-lifetime, thread-safe memory resource because
the graph's allocator need not support concurrent use or retained handles.
