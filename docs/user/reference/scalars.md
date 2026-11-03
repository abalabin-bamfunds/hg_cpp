# Scalar values and schemas

When a complete record or collection should update together, use a scalar type
inside `TS`. HG treats it as one current value, even when it contains multiple fields
or items.

## Built-in mappings

| Python | C++ | Behavior |
| --- | --- | --- |
| `int` | `i64` | Signed 64-bit integer |
| `float` | `double` | Native floating-point value |
| `bool` | `bool` | Boolean |
| `str` | `string` | UTF-8 text at the Python boundary |
| `Symbol` | `util::symbol` | Interned text; null differs from empty text |
| `datetime.datetime` | `date_time_t` | Timezone-naive UTC timestamp with microsecond precision |
| `datetime.timedelta` | `timedelta_t` | Signed 64-bit microseconds |
| `tuple[T, ...]` | `List<T>` | Homogeneous immutable scalar sequence |
| `tuple[A, B]` | `Tuple<A, B>` | Fixed heterogeneous scalar sequence |
| `dict[K, V]` | `Dict<K, V>` | Scalar key/value dictionary |
| `frozenset[T]` | `Set<T>` | Immutable scalar set |

Python mutable `list[T]` annotations are unsupported. Use `tuple[T, ...]`; conversion
accepts tuples and lists when the expected type is known, owns the stored value, and
returns immutable tuples. Optional tuple items are unsupported.

`frozenset[T]` and `typing.FrozenSet[T]` declare native scalar sets. Conversion
accepts Python sets and frozensets when the expected type is known, owns a snapshot,
and returns an immutable frozenset. Items must be hashable scalar types; optional
items and mutable `set[T]` annotations are unsupported. `TS[frozenset[T]]` publishes
a complete scalar set per tick; `TSS[T]` publishes [set changes](input-views.md#collection-views).

Timestamp conversion interprets a timezone-naive `datetime` as UTC:
`datetime(1970, 1, 1)` maps to the Unix epoch. Values before and after the epoch
retain microsecond precision, independently of the machine's timezone.
Conversion rejects timezone-aware values, `date`, and `time` objects.

Python dictionary node arguments use a read-only callback-scoped mapping view. `copy()`
materializes it. C++ `Scalar<T>` borrows a value; `ScalarValue<T>` owns one. Retain
owning values beyond a callback or allocation scope.

## Schemas

To give record fields names and checked types, declare them with Python `@schema`. `Bundle[Schema]`
is a scalar compound value; `TSB[Schema]` projects fields to separate time series. Explicit time
series fields pass through to the TSB projection. Scalar `Optional[T]` fields preserve absence in
the scalar bundle; that optional presence is not projected to TSB fields, which use ordinary time
series validity.

C++ schema templates declare `Field<T>` members. `Bundle<Schema>` supplies the scalar
projection and `TSB<Schema>` the time series projection. Schema identity, field types,
and inheritance determine compatibility, not matching text names alone. Schemas support
single inheritance. Python optional direct self-fields support recursive scalar bundles;
mutually recursive declarations and recursive collections are unsupported.

## Python object scalars

To pass an application object through a graph, use its Python class as a scalar
annotation. Inheritance determines scalar
compatibility; the graph retains the actual Python object by reference. Objects used
this way must be immutable, including state affecting hashing or equality. `None`
remains a no-update sentinel and cannot be stored as an object scalar.

`getitem` infers a Python object's result scalar type from its `__getitem__` return
annotation. `getattr` uses the attribute annotation. Supply `getitem[ResultType](value,
key)` or `getattr[ResultType](value, name)` if inference is unavailable. This bracketed
type is a scalar type; collection time series selection does not take it. Exceptions
from Python equality, hashing, or attribute/item access propagate to the runner.
