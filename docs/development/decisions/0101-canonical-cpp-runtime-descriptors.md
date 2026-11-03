# Canonical C++ runtime descriptors

Status: accepted

Engine-created C++ scalar runtime descriptors must have one identity across shared libraries.
Composed time series runtime descriptors use scalar runtime identity in their
keys. Separate module-local descriptors for the same C++ representation create
conflicting default time series runtimes even when their semantic type is
already canonical.

Canonical lookup uses both the C++ declaration and runtime descriptor type.
This preserves distinct physical representations of the same semantic type.
Recursive descriptors publish their shell before completing their fields.

The [type system](../design/type-system.md#runtime-types) defines runtime
identity and descriptor lifetime.
