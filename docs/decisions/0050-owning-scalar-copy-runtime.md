# Runtime selection for owning scalar copies

Status: accepted

## Decision

`ScalarRuntimeType::runtime_type_for_copy(const ScalarData&) const` selects the destination
representation for an erased owning copy. The default preserves the source
runtime. The storage-trait operation uses the same name and takes priority for
typed copies. BG selects the destination once, then constructs the owning value
using that runtime.

See the [scalar provider contract](../scalar-api.md#scalar-providers).

## Reason

A borrowed scalar can refer to mutable storage while its owning copy requires
an immutable snapshot with a different layout. Erased values retain their
runtime type, so the runtime provides the copy policy when a C++ trait cannot
be selected. A typed provider can still require its declared owning
representation.
