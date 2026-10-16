# Test graph results own recursive deltas

Status: accepted

## Decision

The testing API owns captured output deltas. Scalar leaves and keys use owning
scalar copies with an explicit allocator. Literal expectations are consumed
synchronously and matched directly against recorded operations. See the
[test graph result contract](../testing.md).

## Reason

Results outlive graph storage and must preserve values from earlier cycles.
Separate result storage preserves the borrowed literal contract and the runtime
storage interfaces. Direct comparison preserves every recorded operation,
including empty publications, invalidation, and removal.
