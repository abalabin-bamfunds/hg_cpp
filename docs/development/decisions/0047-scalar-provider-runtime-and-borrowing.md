# Scalar provider runtime selection and borrowing

Status: accepted

## Decision

Canonical runtime selection and borrowing of C++ read views are optional
operations on `scalar_storage_traits<T>`. They use the existing scalar runtime
and borrowed descriptor interfaces.

## Reason

A provider's physical representation determines its runtime operations and how
to interpret its C++ read view. A read view's declaration can expose a base
type while its actual value has a derived type. Owning construction and
publication need that actual type to preserve the complete value.

Keeping these operations on storage traits gives the provider one place to
define representation and lifecycle. Borrowing supplies actual value metadata;
the existing typed copy policy selects the owning representation.

See [Scalar providers](../design/scalar-api.md#scalar-providers) for the contracts.
