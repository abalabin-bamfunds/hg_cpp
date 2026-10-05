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

A time series value stores its latest change state and value independently. Its
runtime type describes the value storage. A never-valid view has `MIN_DT` as
its change time. An invalidated view retains its invalidation time. The packed
state is defined by [time series validity](time-series-validity.md).

`TimeSeriesRuntimeType` is the runtime type used by `In<>` and `Out<>`. It
provides the semantic type, physical layout, contained-item access, and runtime
operations. One semantic time series type can have multiple runtime types,
including normal and free collection layouts.

`In<>` contains a runtime type pointer, last-updated pointer, and data pointer.
`Out<>` is a one-word view of an internal mutation record. References retain the
target runtime type because the target layout can change when the reference
changes.

`In<>` and `Out<>` visitors select value, reference, list, bundle, set,
dictionary, or erased extension access. The selected view is valid for the
visitor callback. Fully specified views use their typed operations directly.

The [provider API](time-series-providers.md) defines how a separate library
adds declarations, runtime representations, and input/output views.

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

## Retaining ticks

`TimeSeriesTickWriter::write(input, now)` stores one value update, invalidation,
or collection delta in a buffer record. The caller supplies the current
evaluation time; the writer records a change only when its `last_updated()`
matches that time. This prevents an older invalidation from being recorded
again when another input causes evaluation. Records contain deltas only, not
snapshots. Replay applies each delta to the existing output and does not clear
a valid collection. Reconstructing a value therefore requires replaying from a
matching baseline, not from an arbitrary later record. Scalar payloads are
owned by the record until replay or discard.

Each record begins with a one-byte header that identifies the update and its
validity. Collection entries follow, and each collection ends with a
standalone `end` header. The first valid tick of an empty TSS or TSD has
a valid root header and an empty collection body. Replay marks an invalid
output valid for this event; it leaves an already-valid output unchanged.

The built-in writer supports value, fixed-list, bundle, set, and dictionary time
series. It does not support references. TSS and TSD themselves cannot be
invalidated through their output APIs, so such an event cannot be retained.
Provider-defined time series register write, replay, and discard handlers
with `TimeSeriesTickDispatch`; the dispatch object must outlive its writer and
reader. When passed to `feedback()`, it must also outlive the compiled graph
and all runtime graphs created from it. Complete registration before
concurrent writes, replay, or graph evaluation; dispatch registration is not
synchronized. Write handlers receive the provider's
`In<Series>` view and a restricted `TimeSeriesTickWriteContext`. Replay and
discard handlers use `util::buffer_reader`; replay receives its
registered output view. Register the complete output declaration with
`add_replay<Output>()` and one replay handler per semantic
type or runtime representation. A runtime registration selects the provider's
typed output view for that representation. Collection replay selects handlers
using each item's actual runtime type. A semantic handler
must support every runtime representation without an exact registration.
Discard receives the semantic type's record format and must destroy provider-
owned objects. Failed writes discard the complete prefix through that same
record grammar, so handlers must write records whose completed prefix can be
discarded. All runtime representations of one semantic time series type must
use the same write and discard grammar.

A fixed provider scalar type can register typed write, replay, and discard
handlers with `TimeSeriesTickDispatch::add_scalar_handlers<T>()`. The handlers
are selected by semantic scalar type and define one record grammar across its
runtime representations. Built-in scalar types keep their compact built-in
encodings. An extension scalar type without registered handlers uses the
owning `ScalarValue<>` fallback; this currently covers both erased values and
unregistered fixed extension types. The scalar view being erased does not imply
that its semantic type is polymorphic.

The replay handler must call its consumer exactly once with a borrowed scalar
view whose lifetime covers that call. Write, replay, and discard must use the
same byte grammar.

The semantic time series type selects the discard handler. Replay first looks
for a handler keyed by the output's runtime representation, then falls back to
one keyed by its semantic type.

Discard handlers must consume valid records without throwing. The reader
discards unread records during destruction and terminates if that cleanup
fails.

Each writer buffer contains records for one semantic time-series type. Construct
its reader with that same type; individual records do not repeat the type header.

The buffer does not destroy owned tick payloads when its final handle is
released. Create a `TimeSeriesTickReader` for a buffer containing records. It
discards unread records on destruction, releasing their scalar payloads.
