# Time series types

## Purpose

The engine provides time series types for values and common collection shapes.
The [terminology dictionary](terminology.md) defines each type. This document
records its notation and change queries.

Collection notation accepts another collection type as `TS_T`.

Every time series type has the recursive guarantee defined by the
[type system](type-system.md#time-series-guarantees).

The [validity design](time-series-validity.md) defines invalidation and
revalidation for these types.

## Type summary

| Type | Notation | Change queries |
| --- | --- | --- |
| Time series value | `TS<T>` | None |
| Time series reference | `REF<TS_T>` | None |
| Time series set | `TSS<T>` | Added and removed items |
| Time series list (fixed size) | `TSL<TS_T, Size>` | Updated items |
| Time series list (variable size, sparse) | `TSL<TS_T>` | Added, removed, and updated items |
| Time series bundle | `TSB` | Updated items |
| Time series dictionary | `TSD<K, TS_T>` | Added and removed keys; modified items |

## Layout

The [TSD layout](tsd-layout.md) defines dictionary storage, shared
representations, and tabular buffers.
