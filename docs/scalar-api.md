# Scalar API

## Purpose

The scalar API separates read-only access from ownership. It also gives generic
node code access to a value without fixing its resolved scalar type.

## Read-only access

`Scalar<T>` is a borrowed, read-only view. Copying it copies the view. Its owner
defines its lifetime. A view supplied to a node is valid for that node
invocation and must not be retained.

A concrete leaf scalar exposes `value()` and `operator*`. When its value is a
class, union, or pointer, `operator->` accesses that value. A collection scalar
exposes its collection operations directly and has no `value()` operation. `Scalar<SCALAR>`
exposes any resolved scalar type accepted by that variable without assuming
equality or hashing. `Scalar<Variable>` exposes either operation only when the
variable requires it. `Scalar<>` is a view erased independently of a type
variable.

`scalar_cast<T>()` checks the resolved type before narrowing a view.
`unchecked_scalar_cast<T>()` requires the caller to prove compatibility. Both
operations retain the source lifetime.

Each scalar view contains a `ScalarRuntimeType` pointer and one data word. The
runtime type provides the semantic type, data access, and operations required
by the view. The data word identifies stored data. An owning value can use that
word for a small inline value. Every `Scalar` and `ScalarValue` is two words.

A scalar type can have more than one runtime type. Copy, assignment, equality,
and keyed lookup operate through both runtime types. Native and composed
collection layouts therefore interoperate without converting a view to a C++
collection object.

The view visitor selects typed built-in atomic access, structural compound,
list, set, or dictionary access, or erased extension access. Generic node code
can use this interface without inspecting the semantic type. The selected view
is valid for the visitor callback.

## Built-in declarations

The built-in atomic declarations are `byte`, `bool`, `i64`, `double`,
`date_time_t`, `timedelta_t`, `string`, and `symbol`. They have these C++
representations:

- `byte` is `std::byte`.
- `i64` is `std::int64_t`.
- `date_time_t` is the existing `EvaluationTime` representation.
- `timedelta_t` is `std::chrono::microseconds`.
- `symbol` is the existing `util::symbol` representation.

`std::byte` is the only source type that canonicalizes to `byte`. Character and
eight-bit integer types do not canonicalize to it. The supported `int` and
`float` declarations remain outside the built-in visitor list and use the
erased visitor fallback.

## Ownership

`ScalarValue<T>` owns its value and is not copy constructible. Moving it
transfers ownership. Construction from `Scalar<T>` copies the current value.
`ScalarValue<>` owns a value of any resolved scalar type. It can copy or replace
its value from any `Scalar` or `ScalarValue`. An owning value directly exposes
its type, equality, and hashing operations when its scalar type supports them.

`ScalarValue<T>::view()` returns a `Scalar<T>`. The returned view remains valid
while the owner retains that value.

`ScalarHash` and `ScalarEqual` provide transparent hashing and equality for
containers that accept any combination of `Scalar` and `ScalarValue` handles.

Use `ScalarValue` when a value must outlive a borrowed view. A constructor can
accept a `bg::allocator` for owned storage.

An explicit allocator defines the ownership of dynamic storage. Without
one, dynamic storage created during a node invocation belongs to the runtime
graph, and its `ScalarValue` must be destroyed in that invocation. This is
required for graph memory accounting.

## C++ source values

`ScalarValue` converts supported C++ source types to the engine's scalar types.
Signed and unsigned integers that fit convert to `std::int64_t`. Character
strings convert to `bg::string`, whose read API returns `std::string_view`.
`EvaluationTime` retains its type.

Wiring applies the same conversion to raw C++ scalar arguments before type
resolution. An explicit `ScalarValue<T>` retains `T`.

`ScalarValue<Opaque<T>>` owns a copy of private C++ data without adding graph
operations. `T` must be copy constructible and destructible. For `Opaque<T*>`,
ownership of the pointed-to object remains with the caller.

## Lists

`List<T>` declares a homogeneous scalar list type. `Scalar<List<T>>` provides
read-only access. `ScalarValue<List<T>>` owns the list and its items.

Node code uses `Scalar<List<T>>` to access a resolved list. Indexing it returns
`Scalar<T>`.

## Collections

`Set<T>` declares a homogeneous scalar set type. `Dict<K, V>` declares a scalar
dictionary type. `Scalar<Set<T>>` and `Scalar<Dict<K, V>>` provide read-only
access. Their storage receives an allocator from its owner without
retaining its pointer. Dictionary keys and values occupy separate columns.

`MutableList<T>`, `MutableSet<T>`, and `MutableDict<K, V>` provide the matching
mutation operations. `State` exposes these views during a node invocation. An
operation that can allocate or release memory uses the active `bg::allocator`.
Looking up a nested collection through a mutable list or dictionary
returns its mutable view.

Generic collection views use the resolved item types. This applies to scalar
parameters and `State` declarations such as `State<Dict<K, SCALAR>>`.

## Compound scalars

A native C++ aggregate is a compound scalar when it is non-empty,
standard-layout, trivially copyable, has at most 32 fields, and each field is
`byte`, `bool`, `i64`, `double`, `date_time_t`, `timedelta_t`, `symbol`, or
another compound scalar.

The C++ type gives a compound scalar its nominal identity. The engine
automatically canonicalizes that identity across libraries and verifies its
native layout and ordered field schema. Each library can instantiate its native
runtime type for the canonical semantic type. Unlike an explicit scalar
provider, a compound scalar does not require an extern template declaration.

`Scalar<T>` exposes the native `T`. Values larger than one word are read by
reference. `Scalar<STRUCT>` exposes ordered field names, types, and scalar
views. Generic field access is allocation-free and does not derive field
addresses from offsets. Equality and hashing process fields and ignore padding.

## Hierarchical scalars

`Scalar<Bundle<Schema>>` exposes the fields declared by `Schema`. `present()`
distinguishes an absent optional field from a present default value. Indexing
returns a borrowed erased scalar view and throws `std::bad_optional_access` for
an absent field.

`bundle<Schema>(arguments...)` constructs a `ScalarValue<Bundle<Schema>>` when
all arguments are scalars. Positional arguments follow inherited logical field
order. Named arguments use `_n`; a positional argument cannot follow a named
argument. Optional fields may be omitted. An overload taking `bg::allocator&`
as its first argument places owned storage under that allocator.

`In<TS<Bundle<Schema>>>::value()` and `operator*` return the borrowed
`Scalar<Bundle<Schema>>` view. `Out<TS<Bundle<Schema>>>` accepts a compatible
scalar view or owning scalar value through `set()`.

A hierarchical scalar value stores one shared handle in its data word. Its
control block stores the actual runtime type, optional-field presence, and
field data. Copying a hierarchical value from a view retains that control block
instead of copying its fields. A view through a base schema preserves the
actual derived runtime type.

Scalar lists and dictionary mapped values store the same one-word handle.
Collection copies retain the control block and preserve its actual runtime
type.

Hierarchical control blocks use a thread-safe allocation owner associated with
the outer global state. A control block retains that owner, so an owning value
can outlive the runtime graph or global state that created it. Releasing the
final value destroys its present fields and releases the owner.

## Scalar providers

A scalar provider defines three independent parts:

- `scalar_type_traits<T>` maps its C++ declaration to one semantic
  `ScalarType` object.
- `scalar_storage_traits<T>` defines its canonical C++ stored object, read
  value, intrinsic equality and hashing, and storage lifecycle.
- `cpp_scalar_runtime_type<T>()` returns the canonical `ScalarRuntimeType`
  generated from those traits.

The provider defines `scalar_type_traits<T>::type()` out of line. It also
explicitly instantiates `cpp_scalar_runtime_type<T>()` in the provider library
and declares that instantiation with `extern template` in its public header.
This gives the semantic type and canonical runtime type one stable identity
across shared-library boundaries.

`scalar_declaration_traits<T>` controls whether graph signatures accept the
declaration as a collection, an implicit scalar argument, or a time series
value. Its defaults describe an ordinary leaf scalar.

## Optional operations

An optional integration stores its handlers in `ScalarDispatch<Handler>`.
The registry accepts exact `ScalarRuntimeType` and exact `ScalarType`
registrations. Lookup by runtime type first checks its exact runtime-type
handler and then its semantic-type handler. Registration does not add fields or
virtual functions to scalar types, runtime types, or values.
Resolved handler pointers remain stable for the registry lifetime and can be
cached while processing values of one runtime type.

Use `Scalar::visit()` for structural list, set, and dictionary handling. An
operation handles built-in atomic types through typed `Scalar<T>` callbacks. It
performs `ScalarDispatch` lookup in the `Scalar<>` extension catch-all callback.
Pass the operation object to a registered handler when the handler must recurse.

Complete registration before concurrent lookup. `ScalarDispatch::add()`
rejects a duplicate exact registration.
