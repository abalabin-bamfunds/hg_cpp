# Exact-type C++ schema field access

Status: accepted

## Decision

Shared schema reflection uses one physical initializer sequence for aggregate
construction and field-offset capture. The sequence includes hidden base
fields and empty-base initializers. Logical access selects the most derived
declaration of each field name.

Field wrappers opt in with an implicit `FieldProbe&` constructor that records
their address and leaves them valid for destruction. Reflection captures
offsets from a local object of the actual schema and wrapper instantiation.
Only the offsets leave that scope. They are cached per instantiation.

## Context

Field constructors affect C++ object layout, including base tail-padding use.
Capturing the actual instantiation is required to locate its field subobjects.
The sample remains local because moving a base or returning a small aggregate
by value can change field addresses before capture reads them.

Direct aggregate construction also removes the requirement to move each base
object. The [type system](../type-system.md#shared-c-schemas) defines the
extension-facing API and logical field order.
