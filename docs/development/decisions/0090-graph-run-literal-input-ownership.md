# Graph-run literal input ownership

Status: accepted

## Context

`GraphRunLiteralInput` can describe nested literals backed by caller-owned scalar
objects and sequence storage. Inline `run_graph` evaluation is synchronous, while
callers can also save a descriptor for later use.

## Decision

Inline literal input factories return a borrowed descriptor reference for use in
the same `run_graph` call. Copying a borrowed descriptor recursively snapshots its
sequences and scalar payloads. Copies of an owned descriptor share the immutable
snapshot; moves transfer it and leave the source empty. Low-level literal copies
remain borrowed. This avoids snapshot allocation for inline inputs while allowing
a saved descriptor to own its input data.

## Consequences

The descriptor distinguishes borrowing during evaluation from ownership when a
caller saves it. Lifetime annotations describe factory and source borrows. Range-
based `ticks(range)` retains its existing range-view ownership rules.

See [graph execution](../../user/reference/execution.md#arguments).
