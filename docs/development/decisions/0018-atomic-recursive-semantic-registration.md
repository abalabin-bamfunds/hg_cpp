# Atomic recursive semantic registration

Status: superseded by [single-threaded type registration](0094-single-threaded-type-registration.md).

## Decision

Recursive hierarchical schemas reserve their canonical identities before they
resolve field types. One registration transaction covers the hierarchical and
scalar collection types created while resolving a connected recursive schema
group. It publishes the complete group atomically or removes every new entry.

Scalar capabilities are finalized across the complete group before hash-key
requirements are validated.

## Consequences

Concurrent first access cannot observe incomplete field metadata. A failed
registration leaves no collection type that refers to a failed hierarchical
type, and a later access can retry registration.

The shared registration lock serializes first construction of scalar semantic
types. Access to an existing canonical type remains a lookup under that lock.
