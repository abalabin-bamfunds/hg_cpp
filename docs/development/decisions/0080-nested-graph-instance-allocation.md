# Nested graph instance allocation

Status: accepted

## Context

`NestedGraph` allocates each `RuntimeGraph` object through the parent runtime
graph's allocator. The nested runtime graph uses its allocator for its internal
storage.

## Decision

Finalize a nested runtime graph before deleting its object through the parent
runtime graph's allocator. Retain a stopped instance through its outer cycle
before finalization and deletion.

## Reason

The parent allocator owns the `RuntimeGraph` object. The nested graph allocator
owns only the instance's internal storage.

See [nested graph lifecycle](../design/nested-graphs.md#lifecycle) and
[deferred cleanup](../design/evaluation.md#deferred-cleanup).
