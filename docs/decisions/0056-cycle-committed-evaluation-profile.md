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
release consumed blocks. The outer cycle opens and closes the root graph scope;
nested graph evaluations use explicit entry and exit records. Graph-start
records carry the compiled graph pointer, parent graph ID, and owner node index
so readers can identify a node without retaining semantic map keys in the
engine. The compiled graph must remain alive while a reader consumes records
that refer to it.

## Consequences

The interval between node checkpoints includes publication, scheduling, and
other work between nodes; it is not an isolated node-function duration.
`RuntimeGraph` stores an 8-byte graph instance ID (88 to 96 bytes in this
build). A failed recording can end without graph-stop records.

See [evaluation profiling](../evaluation.md#evaluation-profiling).
