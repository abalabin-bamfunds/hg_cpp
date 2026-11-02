# Retained tick record cleanup

Status: accepted

## Context

Provider tick handlers can construct owned objects while writing a record. If a
handler fails, those objects must be destroyed without removing earlier
committed records. A per-write ownership journal duplicates the record's
cleanup grammar.

## Decision

On write failure, expose the uncommitted prefix through a private, synchronous
reader and discard it using the time-series record grammar before rolling back
the bytes. Do not retain a separate ownership journal.

## Consequences

Rollback uses the existing discard handlers and adds no per-write journal.
Handlers must make every completed record prefix discardable; cleanup ignores
an incomplete trailing field. The pending reader cannot be forked and must not
be retained beyond cleanup.

See [time-series types](../design/time-series-types.md#retaining-ticks).
