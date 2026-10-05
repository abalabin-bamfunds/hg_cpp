# Shared runtime graph evaluator

Status: accepted

## Context

A high-count map creates one runtime graph per key. The parent connection is
common to those graphs, while runtime storage, scheduling, and lifecycle state
differ per key.

## Decision

Store the common owner connection and lifecycle execution in `RuntimeGraphEvaluator`. A
`map` or `mesh` owner shares one evaluator across its runtime graphs. Each
`RuntimeGraph` retains its own allocator, storage, scheduling, and lifecycle
state and owner-supplied instance token. The evaluator keeps one owner callback
connection for those instances. The top-level caller owns `GlobalState`.
Evaluation contexts carry it during execution and inner-graph finalization. The
outer runtime graph borrows it for finalization after the last evaluation
context ends.

## Consequences

On 64-bit targets, each map or mesh branch uses a 64-byte `RuntimeGraph` instead
of 112 bytes. One 32-byte evaluator serves the owner. Scheduling continues to
use graph-local node indices and storage.

See [nested graph evaluation](../nested-graphs.md#evaluation) and
[global state](../global-state.md).
