# Collection representation runtime identity

Status: accepted

## Context

Collection representations may read values stored with provider-specific
runtime layouts. Using those layouts to identify representation runtime types
creates descriptors for incidental storage choices and lets the descriptor
cache grow with source layouts.

## Decision

Key collection representation runtime descriptors by their semantic type and
adaptation plan. TSD descriptors derive key and item metadata from the declared
representation type; routed reference items use the runtime type for routed
storage. TSL and TSB plans retain adaptation modes and use canonical requested
field runtime types.

Collection reads return the runtime type that describes the storage actually
returned. This allows direct and adapted fields to retain provider-specific
layouts without making those layouts part of representation descriptor
identity.

## Consequences

Equivalent representations reuse runtime descriptors across source layouts.
Readers must use the runtime type returned with each item instead of inferring
its storage layout from the collection descriptor.

See [time series references](../design/time-series-references.md#dynamic-representations).
