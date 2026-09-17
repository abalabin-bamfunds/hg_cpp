# Time series types

## Purpose

The engine provides time series types for values and common collection shapes.
The [terminology dictionary](terminology.md) defines each type. This document
records its notation and change queries.

Collection notation accepts another collection type as `TS_T`.

Every time series type has the recursive guarantee defined by the
[type system](type-system.md#time-series-guarantees).

A guarantee is an optional template property:

```cpp
bg::TS<float, bg::TimeSeriesGuarantee::final>
```

The [validity design](time-series-validity.md) defines invalidation and
revalidation for these types.

TSS and TSD validity starts with their first tick and then remains valid. TSD
item validity remains independent.

## Type summary

| Type | Notation | Change queries |
| --- | --- | --- |
| Time series value | `TS<T>` | None |
| Time series reference | `REF<TS_T>` | None |
| Time series set | `TSS<T>` | Added and removed items |
| Time series list (fixed size) | `TSL<TS_T, Size>` | Updated items |
| Time series bundle | `TSB<Schema>` | Updated items |
| Time series dictionary | `TSD<K, TS_T>` | Added and removed keys; modified items |

`TSL<TS_T>` declares the semantic type for a variable-size sparse list. Its
runtime storage and node views are not implemented.

## Layout

A time series value stores its last updated time and value independently. Its
runtime type describes the value storage. An invalid view points to `MIN_DT` as
its last updated time.

`TimeSeriesRuntimeType` is the runtime type used by `In<>` and `Out<>`. It
provides the semantic type, physical layout, contained-item access, and runtime
operations. One semantic time series type can have multiple runtime types,
including normal and free collection layouts.

`In<>` contains a runtime type pointer, last-updated pointer, and data pointer.
`Out<>` is a one-word view of an internal mutation record. References retain the
target runtime type because the target layout can change when the reference
changes.

`In<>` and `Out<>` visitors select value, reference, list, bundle, set, or
dictionary access. The selected view is valid for the visitor callback. Fully
specified views use their typed operations directly.

Collection storage uses the resolved runtime type of each contained time
series. Runtime-defined collection types therefore support the same contained
time series as C++-defined collection types.

Collection runtime types identify items with 32-bit item IDs. TSL and TSB use
the item position. TSD uses its stable sparse-storage position and reserves
`key_set_item_id` for its key set. Item enumeration returns current collection
items. Packed output storage also provides each item's storage layout.

TSD input can enumerate keys, item IDs, values, key-value pairs, or ID-value
pairs. Added, removed, and modified queries enumerate keys for the current
change.

The [TSD layout](tsd-layout.md) defines dictionary storage and reference
representations.
