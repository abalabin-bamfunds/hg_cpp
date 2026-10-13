# Cycle-committed evaluation profile

Status: accepted

## Context

Production profiling must expose per-node work without changing graph evaluation
when the recording buffer fails. A monitoring consumer may read on another thread.

## Decision

The evaluator selects a plain or recorded path before its node loop. The
recorded path writes one clock checkpoint after each completed evaluation request,
before output publication. It commits records only after the outer cycle
completes. An incomplete outer cycle discards its uncommitted records and ends
recording; later evaluation uses the plain path.

The caller owns the recording buffer. A reader may poll committed records and
release consumed blocks. Graph-instance IDs and compiled-definition metadata
identify nodes without retaining semantic map keys in the engine.

## Consequences

The interval between node checkpoints includes publication, scheduling, and
other work between nodes; it is not an isolated node-function duration.
Recording does not add storage to `RuntimeGraph`. A failed recording can end
without graph-stop records.

See [evaluation profiling](../evaluation.md#evaluation-profiling).
