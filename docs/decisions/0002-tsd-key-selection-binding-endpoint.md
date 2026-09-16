# TSD key selection binding endpoint

Status: accepted

## Context

Selecting a TSD item produces a reference whose target can change when the key
is added, removed, restored, or assigned a new reference. The runtime type must
remain immutable and reusable.

## Decision

Each selected key owns its current `Reference` and exposes a separate binding
endpoint. A dynamic `REF` input stores that endpoint, subscribes to it, and
compares the endpoint's selected reference before scheduling its node.

Value items use their item reference as the selected reference. Reference items
store the selected reference and notify the endpoint directly. Both use the
canonical `REF<V>` runtime type.

Unused selections are reclaimed by an outer-cycle callback. This keeps a
temporary selection valid through the evaluation that created it.

## Consequences

Runtime types contain no mutable routing state. Reference-item target updates
bypass the selecting node. Selection maintenance is proportional to affected
keys and active bindings.
