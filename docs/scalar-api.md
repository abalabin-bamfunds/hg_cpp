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
list, set, or dictionary access, or erased extension access. A
`BuiltinScalarType` carries the built-in declaration index used to select a
typed atomic view. Other leaf scalar types use the erased view. Generic node code
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

`string` stores immutable UTF-8 bytes in a reference-counted `StringStorage`.
Its scalar and time-series read value is `std::string_view`. Construction from
a `std::string_view` copies the bytes; copying a view of canonical string
storage retains the same storage. A string is limited to 4,294,967,295 bytes
(4 GiB minus one). Construction and builder growth reject larger values. The engine does not validate or
transcode UTF-8.

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

String storage retains its allocation owner until the last handle is released.
The allocator's upstream memory resource must still outlive every string value
allocated from it. `StringBuilder` appends text into allocator-owned storage and
`finish()` transfers that storage into a `ScalarValue<string>`.

## C++ source values

`ScalarValue` converts supported C++ source types to the engine's scalar types.
Signed and unsigned integers that fit convert to `std::int64_t`. Character
strings convert to `bg::string`, whose read API returns `std::string_view`.
`EvaluationTime` retains its type.

Wiring applies the same conversion to raw C++ scalar arguments before type
resolution. An explicit `ScalarValue<T>` retains a concrete `T` as its semantic
type. For a generic declaration, wiring uses the value's resolved type.

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

`Scalar<Dict<K, V>>::find(key)` returns an optional borrowed `Scalar<V>` and
performs one key lookup. A dictionary cursor returns key and mapped-value
storage pointers in caller-provided batches. The cursor borrows the dictionary;
keep the dictionary alive and unchanged until the cursor is destroyed. The
batch contains storage pointers, interpreted with the dictionary's key and
mapped runtime types. Creating a cursor allocates one runtime-specific cursor
object; advancing it uses the caller's batch buffer.

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

`Scalar<Bundle<Schema>>` exposes the fields of the actual value through its
runtime type. A base annotation controls compatibility; field inspection
includes derived fields and their actual types.
`Scalar<HIERARCHICAL>` provides the same structural access for any hierarchical
scalar. `present()` distinguishes an absent optional field from a present
default value. Indexing returns a borrowed erased scalar view and throws
`std::bad_optional_access` for an absent field.

`Scalar<Bundle<SCHEMA>>` provides this access for any resolved bundle.
Its view and an owning `ScalarValue<Bundle<SCHEMA>>` retain the actual runtime
type. Borrowing copies no fields. Composed bundle ownership retains the existing
shared handle.

For example, a node can forward a bundle with a captured schema:

```cpp
void forward_bundle(In<TS<Bundle<SCHEMA>>> input,
                    Out<TS<Bundle<SCHEMA>>> output) {
    output.set(input.value());
}
```

`bundle<Schema>(arguments...)` constructs a `ScalarValue<Bundle<Schema>>` when
all arguments are scalars. Positional arguments follow inherited logical field
order. Named arguments use `_n`; a positional argument cannot follow a named
argument. Optional fields may be omitted. An overload taking `bg::allocator&`
as its first argument places owned storage under that allocator. Construction
uses the engine's composed hierarchical runtime type. Extension runtime types
provide their own construction APIs.

`bundle(schema, fields)` constructs a `ScalarValue<Bundle<SCHEMA>>` from a
runtime schema. `fields` is a `std::span<const std::optional<Scalar<>>>` in
flattened schema order. The span must contain one entry per field; an empty
entry is allowed only for an optional field. Each present value must have a
compatible runtime type. `bundle(allocator, schema, fields)` selects an explicit
allocator. Both forms use the same composed construction as C++ bundles.

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

List and dictionary runtime types copy and assign compatible collection values
by constructing each destination item. This supports covariance through list
items and dictionary mapped values. Dictionary keys remain invariant. Runtime
storage never reinterprets one collection declaration as another.

Set runtime input remains exact. A set indexes its items by their hash, and
runtime input compatibility does not guarantee that conversion preserves the
destination equality and hash behavior.

Hierarchical control blocks use a thread-safe allocation owner associated with
the outer global state. A control block retains that owner, so an owning value
can outlive the runtime graph or global state that created it when their
upstream memory resource remains alive. Releasing the final value destroys its
present fields and releases the owner.

Construction outside an allocation context requires an explicit allocator.
The allocator's memory resource must outlive every value constructed from it.

## Scalar providers

A scalar provider defines three independent parts:

- `scalar_type_traits<T>` maps its C++ declaration to one semantic
  `ScalarType` object.
- `scalar_storage_traits<T>` defines its canonical C++ stored object, read
  value, intrinsic equality and hashing, and storage lifecycle.
- `cpp_scalar_runtime_type<T>()` returns the canonical `ScalarRuntimeType`
  selected or generated from those traits.

A provider can supply its canonical runtime by adding
`static const ScalarRuntimeType &runtime_type(const ScalarType &type)` to
`scalar_storage_traits<T>`. `cpp_scalar_runtime_type<T>()` calls this hook with
the declaration's semantic type and registers the returned runtime. The runtime
must identify that semantic type, match the declared stored representation,
and have process lifetime. Repeated requests return the same canonical runtime.
Without the hook, BG selects or generates its existing canonical runtime.

The provider defines `scalar_type_traits<T>::type()` out of line. It also
explicitly instantiates `cpp_scalar_runtime_type<T>()` in the provider library
and declares that instantiation with `extern template` in its public header.
This gives the semantic type and canonical runtime type one stable identity
across shared-library boundaries. Semantic type factories follow the
[type registration contract](type-system.md#type-model).

`scalar_declaration_traits<T>` controls whether graph signatures accept the
declaration as a collection, an implicit scalar argument, or a time series
value. Its `generic` flag identifies declarations containing type variables.
Providers enumerate those variables through
`declaration_variable_traits<T>::for_each`. The defaults describe a concrete
ordinary leaf scalar.

### Alternative runtime types

A provider implements another physical representation by deriving from
`ScalarRuntimeType`. Its virtual methods use `ScalarData` and `ScalarDataMode`
from `<bg/scalar/data.hpp>`. These are APIs for extension authors implementing
runtime types. Node code uses `Scalar`, `ScalarValue`, `In`, and `Out`.

`ScalarData` contains a runtime-type pointer, a payload word, and its mode.
Copying or destroying this descriptor does not retain or release its payload.
A runtime callback borrows the descriptor and payload for the call. Retaining
a value requires an owning scalar or the representation's shared owner. The
runtime type must outlive every descriptor that refers to it.

| Mode | Payload |
| --- | --- |
| `stored_view` | Pointer to borrowed runtime storage. |
| `source_view` | Pointer to a borrowed canonical C++ read value. |
| `inline_value` | Value bytes stored in the payload word. |
| `owned_explicit_allocator`, `owned_context_allocator`, `owned_default_allocator` | Pointer to runtime storage owned by the enclosing scalar handle, with the indicated allocator source. |
| `shared_view`, `shared_value` | Representation-defined shared handle; the enclosing scalar handle retains it only for `shared_value`. |

`data_word()` returns the payload word. `data()` returns its address for inline
and shared modes, and the payload pointer for the other modes. Providers must
interpret the payload using its mode. The default scalar read path calls the
source runtime's `read_value(mode, data_word, destination)` into aligned,
uninitialized storage for the provider's declared C++ read value, then destroys
that value after use.

A provider can specialize how a typed view reads runtime representations by
adding `static value_type read_value(const details::ScalarData &source)` to
`scalar_storage_traits<T>`. The hook returns exactly the trait's `value_type`
and interprets the source runtime, payload, and mode. It handles the canonical
runtime directly when that runtime delegates reads back through the trait; it
must not call `source.runtime_type().read_value(...)` unconditionally in that
case. These trait hooks are extension APIs; node code uses `Scalar` and
`ScalarValue`.

A provider can also add
`static const ScalarRuntimeType &copy_runtime_type(const details::ScalarData &source)`.
This selects the runtime type used when `ScalarValue<T>` copies a source. If it
returns the source runtime, BG uses the existing actual-runtime copy or shared
retain path. Otherwise, BG constructs the selected runtime from the source;
that runtime must accept the source representation. When the hook is absent,
typed scalar copies keep their existing behavior, including hierarchical
runtime preservation.

A provider can describe the actual value behind a C++ read view by adding
`static ScalarData borrow_value(const value_type &value)` to
`scalar_storage_traits<T>`. The descriptor identifies the borrowed value's
actual semantic type and physical representation. A Base-annotated view can
therefore carry a Derived value through owning construction and time series
publication. Typed owning construction applies the same copy policy as copying
a `Scalar<T>`. Publication uses the output runtime's input assignment operation.
Sources constructible as `value_type` use this borrowed path without requiring
a separate trait `construct` overload.

The hook borrows the view or its source owner. It must not return a pointer to
a temporary created inside the hook or transfer ownership through the
descriptor. BG keeps a converted C++ view alive through the synchronous copy
or assignment. Without the hook, a C++ read view uses its declaration's
canonical runtime. These operations are extension APIs; node code uses the
ordinary scalar and time series APIs.

A runtime type for a list, set, or dictionary semantic type derives from the
matching collection runtime interface. Its component runtime types correspond
to the semantic type components. Generic collection access and runtime input
compatibility rely on this registration invariant. A dictionary runtime type
also implements one-lookup `find()` and creates a resumable cursor that fills
caller-owned entry batches.

## Optional operations

An optional integration stores its handlers in `ScalarDispatch<Handler>`.
The registry accepts exact `ScalarRuntimeType` and exact `ScalarType`
registrations. Lookup by runtime type first checks its exact runtime-type
handler and then its semantic-type handler. Registration does not add fields or
virtual functions to scalar types, runtime types, or values.
Resolved handler pointers remain stable for the registry lifetime and can be
cached while processing values of one runtime type.

Use `Scalar::visit()` for structural list, set, dictionary, compound, and
hierarchical handling. An operation handles built-in atomic types through typed
`Scalar<T>` callbacks. It performs `ScalarDispatch` lookup in the `Scalar<>`
extension catch-all callback. Pass the operation object to a registered handler
when the handler must recurse.

Complete registration before concurrent lookup. `ScalarDispatch::add()`
rejects a duplicate exact registration.

`ScalarBufferOperations` is the shared optional operation set for scalar values
stored in buffers. It registers independent write, read, and discard handlers
through `ScalarDispatch<ScalarBufferOperations>::instance()`. Scalar extensions
can use this registry for any buffer-backed facility; retained tick storage is
one consumer. Write handlers receive `ScalarBufferWriteContext`, which exposes
typed emplacement and byte writes without exposing the underlying buffer
writer. A runtime registration can provide only the actions that depend on
that representation. A caller can use `find_exact()` to select each action
independently and fall back to the semantic registration when that action is
absent.
