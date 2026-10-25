# Locality-preserving compilation order

Status: accepted

## Context

Topological ordering is required so that each node runs after its input
providers. A global ready queue can interleave unrelated graph regions and
reduce locality between nodes that were wired together.

## Decision

Compilation scans nodes in wiring order. For each node, it recursively emits
each unemitted input provider in signature-input order before emitting that
node. It rejects cycles during this traversal.

`GraphWiring` can add a no-payload ordering constraint from the direct
consumers of a wiring point to a designated node. Compilation treats those
consumers as predecessors of that node. The constraint adds no input or runtime
storage.

## Consequences

The compiled order is topological and follows wiring order where dependencies
do not constrain it. Runtime scheduling continues to process compiled node
indices in increasing order. Cycle diagnostics list the full wiring paths of
the nodes in the cycle.

See [evaluation order](../design/evaluation.md#evaluation-order).
