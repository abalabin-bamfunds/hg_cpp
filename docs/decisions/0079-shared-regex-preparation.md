# Shared regex preparation

Status: accepted

## Context

Regex patterns and replacement templates are known during wiring. ICU compiled
patterns are immutable, but matchers carry mutable search state. A compiled
graph can be used by several runtime instances.

## Decision

Prepare patterns and replacement templates during wiring and retain them in the
compiled graph. Use content-based scalar equality and hashing so equal prepared
values can deduplicate wired nodes. Each runtime node instance creates and owns
its matcher state.

## Reason

Sharing prepared values avoids repeated ICU compilation. Runtime-local matchers
prevent mutable search state from crossing node or runtime-instance boundaries.

See [regular expressions](../library.md#regular-expressions).
