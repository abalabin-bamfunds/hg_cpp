# Scalar values and schemas

A scalar is one current value, including a compound object or scalar collection. Use
scalar types inside `TS` when the complete value should update together.

## Built-in mappings

| Python | C++ | Behavior |
| --- | --- | --- |
| `int` | `i64` | Signed 64-bit integer |
| `float` | `double` | Native floating-point value |
| `bool` | `bool` | Boolean |
| `str` | `string` | UTF-8 text at the Python boundary |
| `Symbol` | `util::symbol` | Interned text; null differs from empty text |
| `datetime.datetime` | `date_time_t` | Timezone-naive local timestamp with microsecond precision |
| `datetime.timedelta` | `timedelta_t` | Signed 64-bit microseconds |
| `tuple[T, ...]` | `List<T>` | Homogeneous immutable scalar sequence |
| `tuple[A, B]` | `Tuple<A, B>` | Fixed heterogeneous scalar sequence |
| `dict[K, V]` | `Dict<K, V>` | Scalar key/value dictionary |

Python mutable `list[T]` annotations are unsupported. Use `tuple[T, ...]`; conversion
accepts tuples and lists when the expected type is known, owns the stored value, and
returns immutable tuples. Optional tuple items are unsupported. Python scalar set
annotations are unsupported. C++ supports scalar sets and timestamp arithmetic.

Timestamp conversion uses Python's timezone-naive local `datetime` contract.
Conversion rejects timezone-aware values, `date`, and `time` objects. Values
are preserved to microsecond precision.

Python dictionary node arguments use a read-only callback-scoped mapping view. `copy()`
materializes it. C++ `Scalar<T>` borrows a value; `ScalarValue<T>` owns one. Retain
owning values beyond a callback or allocation scope.

## Schemas

Python `@schema` declares named fields. `Bundle[Schema]` is a scalar compound value;
`TSB[Schema]` projects fields to separate time series. Explicit time series fields pass
through to the TSB projection. Scalar `Optional[T]` fields preserve absence in the
scalar bundle; that optional presence is not projected to TSB fields, which use ordinary
time series validity.

C++ schema templates declare `Field<T>` members. `Bundle<Schema>` supplies the scalar
projection and `TSB<Schema>` the time series projection. Schema identity, field types,
and inheritance determine compatibility, not matching text names alone. Schemas support
single inheritance. Python optional direct self-fields support recursive scalar bundles;
mutually recursive declarations and recursive collections are unsupported.

## Python object scalars

Other Python classes can be scalar annotations. Inheritance determines scalar
compatibility; the graph retains the actual Python object by reference. Objects used
this way must be immutable, including state affecting hashing or equality. `None`
remains a no-update sentinel and cannot be stored as an object scalar.

`getitem` infers a Python object's result scalar type from its `__getitem__` return
annotation. `getattr` uses the attribute annotation. Supply `getitem[ResultType](value,
key)` or `getattr[ResultType](value, name)` if inference is unavailable. This bracketed
type is a scalar type; collection time series selection does not take it. Exceptions
from Python equality, hashing, or attribute/item access propagate to the runner.
