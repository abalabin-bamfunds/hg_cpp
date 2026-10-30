# Switch output reference compatibility

Status: accepted

## Context

Switch branches can produce compatible collections with different contained
reference types or free and bound storage. Their common output must support the
same reference representations as ordinary input binding.

Fixed collection bindings use positional item IDs. A common bundle schema must
preserve the flattened field order.

## Decision

Switch output inference removes references recursively from each branch target
before computing the common target type. Consumers request their required
representation through normal reference binding.

Named and unnamed bundles with the same flattened field names and order can
share a common type. That type retains the named schema's declaration,
inheritance, and metadata, and combines corresponding field types.

See [switch output inference](../design/switch.md#selection-and-output) and the
[common type contract](../design/type-system.md#time-series-guarantees).
