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
tuple, list, set, or dictionary access, or erased extension access. A
`BuiltinScalarType` carries the built-in declaration index used to select a
typed atomic view. Other leaf scalar types use the erased view. Generic node code
can use this interface without inspecting the semantic type. The selected view
is valid for the visitor callback.

`Scalar<>::accept(ScalarVisitTypes&)` is the engine entry point for a
caller-owned DDV mux, which can be reused across values. `Scalar::visit()`
constructs a DDV adapter and delegates dispatch to it.

`Scalar<>::from(const ScalarData&)` is an engine and provider API. It requires a
non-empty descriptor and preserves its runtime type, mode, and payload. The
view retains no owner; referenced storage must remain valid while it is used.

## Built-in declarations

The built-in atomic declarations are `byte`, `bool`, `i64`, `double`,
`date_time_t`, `timedelta_t`, `string`, `bytes`, and `symbol`. They have these C++
representations:

- `byte` is `std::byte`.
- `i64` is `std::int64_t`.
- `date_time_t` is the existing `EvaluationTime` representation.
- `timedelta_t` is `std::chrono::microseconds`.
- `bytes` is `std::span<const std::byte>`.
- `symbol` is the existing `util::symbol` representation.

`string` stores immutable UTF-8 bytes in a reference-counted `StringStorage`.
Its scalar and time-series read value is `std::string_view`. Construction from
a `std::string_view` copies the bytes; copying a view of canonical string
storage retains the same storage. A string is limited to 4,294,967,295 bytes
(4 GiB minus one). Construction and builder growth reject larger values. The
engine does not validate or transcode UTF-8.

`bytes` stores immutable contiguous binary data in reference-counted storage.
Its scalar and time-series read value is `std::span<const std::byte>`. Construction
from a span copies its bytes; copying a view of canonical byte storage retains
the same storage. `BytesBuilder` provides writable tail storage and commits the
number of bytes written. Its writable span remains valid until the builder
reserves more storage, appends, is moved, or is finished. `finish()` transfers
the completed value into a `ScalarValue<bytes>`.

`bytes` is distinct from `List<byte>`. A byte span canonicalizes to `bytes`.
The list stores byte scalar items; the byte value stores one contiguous binary
payload.

`std::byte` is the only source type that canonicalizes to `byte`. Character and
eight-bit integer types do not canonicalize to it. The supported `int` and
`float` declarations remain outside the built-in visitor list and use the
erased visitor fallback.

## Scalar formatting

`<bg/scalar/format.hpp>` formats scalar views directly into fmt's format
context. For example, `fmt::format("{}", value.view())` formats an owning
scalar value. Strings and symbols use quoted debug formatting; an empty symbol
formats as `""`. Bytes use `bytes("...")` with two lowercase hexadecimal
digits per byte. Date-time values use UTC `YYYY-MM-DDTHH:MM:SS.ffffffZ`.
Durations use `HH:MM:SS.ffffff`, with a leading `-` for negative values and a
`<days>d ` prefix when the whole-day component is nonzero; the sign precedes
the day prefix. Other atomic values use fmt's default representation.

Lists use `[...]`, sets use `{...}` or `set()` when empty, and dictionaries,
structs, and hierarchical scalars use `{key: value, ...}`. Positional tuples
use `(value, ...)`, including the trailing comma for a one-item tuple. Nested
values use the same formatting recursively. A scalar extension registers
`ScalarFormatOperations` with `ScalarDispatch<ScalarFormatOperations>`; an
exact runtime registration takes precedence over the semantic type
registration. Complete registration before formatting on concurrent threads.

## Ownership

`ScalarValue<T>` owns its value and is not copy constructible. Moving it
transfers ownership. Construction from `Scalar<T>` copies the current value.
`ScalarValue<>` owns a value of any resolved scalar type. It can copy or replace
its value from any `Scalar` or `ScalarValue`. An owning value directly exposes
its type, equality, and hashing operations when its scalar type supports them.

`ScalarValue<T>::view()` returns a `Scalar<T>`. The returned view remains valid
while the owner retains that value.

Engine consumers use `std::move(value).move_to(runtime_type, destination)` to
consume an owning value into initialized scalar storage. The destination runtime type selects
storage transfer or compatible assignment. Transfer preserves owning copies of
the previous value. The active scalar allocator must own the destination's
dynamic storage.
Successful assignment empties the owner. Assignment failure leaves the owner unchanged.

`ScalarHash` and `ScalarEqual` provide transparent hashing and equality for
containers that accept any combination of `Scalar` and `ScalarValue` handles.

Use `ScalarValue` when a value must outlive a borrowed view. A constructor can
accept a `bg::allocator` for owned storage.

`ScalarConstruction` is an engine and scalar-provider interface for constructing
one owned value in runtime-selected storage. `ScalarValue<T>::from_construction`
selects the runtime type for a concrete scalar declaration. The erased
`ScalarValue<>::from_construction(runtime_type, construction)` form uses an
explicit runtime type. Both forms accept an optional allocator.

The callback runs synchronously with the destination runtime type, uninitialized
storage sized and aligned for that type, and the owning allocator. It must fully
construct the value before returning. If it throws, the storage must contain no
live object or owned resources. The callback context is borrowed and must remain
valid until it returns. `ScalarListRuntimeType::emplace_back_with` applies the
same callback contract to an item slot and commits the slot only after successful
construction.

`ScalarRuntimeType::assign_with(destination, construction, suppress_equal)`
constructs a replacement with the active scalar allocator and the same callback
contract. It returns `false` when equality suppression skips assignment and
`true` after assignment. Construction or comparison failure preserves the
existing value and releases candidate storage. Native and composed collection
runtimes construct items once and swap their storage headers. The default provider
implementation allocates runtime-sized, aligned candidate storage and calls
`assign_from_input`; this fallback can copy the completed scalar once more.

`ScalarDictRuntimeType::try_emplace_with(storage, memory, key, mapped)` uses
these callbacks to construct a key and mapped value in their final slots. It
hashes the constructed key using the destination key runtime. A duplicate key
returns `false`, destroys the candidate key, and skips mapped construction.
Success returns `true`. An exception destroys the partial entry and preserves
existing entries. Storage can retain allocation capacity after a failed insertion.

`ScalarSetRuntimeType::insert_with(storage, memory, construction)` constructs
an item in its final slot before hashing and duplicate lookup. A duplicate
returns `false` and destroys the candidate. An exception destroys the candidate
and preserves existing items.

`ScalarFieldConstruction` constructs one field or tuple item by index. Its callback
receives the field runtime type, sized and aligned uninitialized storage, and
the allocator. Return `true` after complete construction. Return `false` for
an absent field. A `false` result or exception must leave no live object or
owned resources in that field's storage. The callback runs synchronously and
borrows its context.

`StructScalarRuntimeType::construct_fields_with` and
`HierarchicalScalarRuntimeType::construct_fields_with` invoke field callbacks
in schema order and destroy completed fields on failure. Compound scalars
require every field; hierarchical scalars allow absent optional fields.
Native compounds stage their trivial fields in fixed stack storage, then
initialize the aggregate once. Field callback storage may be temporary.
Composed hierarchical scalars construct fields in final storage and publish
the shared handle after successful construction. Construction reuses the
active allocation owner when the allocator uses the context's memory resource
or the owner's pool resource. A different resource creates a separate owner.
Nested fields use their owner's pool, so a retained child can outlive its parent.

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

An unregistered C++ object can bind directly to a fixed `Opaque<T>` parameter
when its source type is exactly `T`. This conversion requires a concrete
parameter and does not apply to generic or variadic parameters.

`ScalarValue<Tuple<...>>` accepts `std::pair` and `std::tuple` sources whose
items match the declared tuple items. `ScalarValue<List<Tuple<...>>>` accepts a
contiguous range of those sources and owns each constructed item.

`ScalarValue<Opaque<T>>` and `TS<Opaque<T>>` own copies of private C++ data.
`T` must be copy constructible and destructible. For `Opaque<T*>`, ownership
of the pointed-to object remains with the caller.
Opaque scalars support equality and hashing when their `scalar_storage_traits`
specialization provides those capabilities. Both are unsupported by default.

## Literals

`<bg/literal/scalar_literal.hpp>` defines `literal::ScalarLiteral` as
`literal::Literal<literal::ScalarLiteralItem>`. `literal::TimeSeriesLiteral` names
the same type. Scalar construction and time series application interpret this
carrier through separate operations. A scalar item borrows a C++ source,
`Scalar`, `ScalarValue`, or `ScalarData`. A sequence borrows its recursive
initializer-list storage. `Literal` allocates no owning tree.

Consume a literal in the full expression that creates its initializer lists and
temporary sources. Rvalue sources remain alive through that expression. Retaining
a literal does not extend their lifetime. Scalar views delivered by its DDV
visitor are callback-scoped. Extension behavior belongs to the visitor consumer
and the source's scalar storage traits.

`<bg/scalar/literal.hpp>` provides `ScalarValue<T>::from_literal(literal)` and
`ScalarValue<>::from_literal(runtime_type, literal)`. Both return an owning
`ScalarValue` and have overloads taking an allocator as the final argument.
The same construction accepts `Literal<Leaf>` when the leaf provides scalar
views through `visit_scalar()`. The leaf rejects alternatives that do not
represent a scalar source.
The scalar consumer interprets sequences through DDV dispatch. Destination
runtime types construct and destroy storage through source-neutral callbacks:

| Destination | Sequence syntax | Conditions |
| --- | --- | --- |
| List | `{item, ...}` | Items follow sequence order. |
| Tuple | `{item, ...}` | Item count equals the tuple length; each position uses its declared type. |
| Set | `{item, ...}` | Equal items are deduplicated. |
| Dictionary | `{{key, value}, ...}` | Entries have exactly two values; duplicate keys are rejected. |
| Compound scalar or bundle | `{{name, value}, ...}` | Names are strings or symbols; each supplied name is unique and known. |

Named fields can appear in any order. Required fields must be supplied; omitted
optional bundle fields remain absent. Malformed sequences throw
`std::invalid_argument`.

Providers support extension sequences by registering `ScalarLiteralOperations`
from `<bg/scalar/literal_operations.hpp>` with
`ScalarDispatch<ScalarLiteralOperations>::instance()` using the
[optional operation registry](#optional-operations). The handler receives the complete
`ScalarLiteral`, destination runtime type, uninitialized destination storage, and
allocator. It follows the [scalar construction contract](#ownership): return
with one complete value, or throw after releasing any resources it acquired.
Scalar leaves use the destination runtime's ordinary input construction.
Scalar time series replacements pass the same carrier to scalar construction,
including provider sequence handlers.
The extension callback accepts `ScalarLiteral`; generic `Literal<Leaf>` sources
retain the built-in sequence rules above. Missing handlers throw
`std::invalid_argument`.

Bundle sequence construction outside a scalar allocation context requires an
explicit allocator, including bundles nested in collections. See the
[ownership contract](#ownership).

```cpp
auto values = bg::ScalarValue<bg::List<bg::List<bg::i64>>>::from_literal({{1, 2}, {}, {3}});
```

### Sentinels

`<bg/literal/sentinel.hpp>` defines `literal::Sentinel` and `literal::TaggedLiteral`.
Each `Sentinel` instance is a noncopyable instruction with a diagnostic symbol
name. Sentinel equality compares object addresses. Names support diagnostics.
BG exports `SKIP`, `INVALID`, `REMOVE`, and `REMOVED` as singleton instances.
`ScalarValue<Sentinel>` stores and copies a singleton pointer.

```cpp
const literal::Sentinel *instruction = literal::Sentinel::from(literal);
literal::TaggedLiteral call = literal::REMOVED(key_literal);
```

`Sentinel::operator()` creates a `TaggedLiteral` that borrows the sentinel and
argument. Copying a tagged literal copies those references. The caller keeps
both objects alive while a literal or consumer refers to them.

`Sentinel::from(literal)` returns the root singleton pointer for a bare sentinel.
An ordinary scalar, tagged literal, or sequence returns `nullptr`. It does not
traverse children. `TaggedLiteral::from(literal)` returns
`std::optional<TaggedLiteral>`. A present result borrows the sentinel and
argument; the function does not traverse children. Consumers compare sentinel
instances with `==` and interpret only the instructions and tagged arguments
they support.

Sentinel instances have static storage duration. Extensions export one instance
per instruction from their library so scalar values can safely retain its
address:

```cpp
// Extension header; EXAMPLE_EXPORT is the extension's export macro.
extern EXAMPLE_EXPORT const literal::Sentinel RESET;

// Extension source.
const literal::Sentinel RESET{bg::symbol{"example_extension.reset"}};
```

Sentinels and tagged literals have canonical scalar types and runtimes. They do
not support implicit graph arguments or time series declarations. Time series
instruction behavior is defined by the [output API](node-api.md#output-parameters).

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

## Scalar tuples

`Tuple<T...>` declares a fixed-length positional scalar. The items can have
different scalar types. `Scalar<Tuple<T...>>::get<Index>()` returns a borrowed
`Scalar` view with that position's declared type. `Scalar<TUPLE>` provides
runtime-indexed item access for any resolved tuple. Equality and hashing require
the respective operation on every item.

```cpp
auto value = ScalarValue<Tuple<string, i64, symbol>>::from_literal(
    {"answer", 42, symbol{"ok"}});
auto count = value.view().get<1>().value();
```

Tuple literals provide exactly one value per position, including zero values
for `Tuple<>`. Construction uses each position's runtime type and the existing
`ScalarFieldConstruction` callback contract. Completed items are destroyed if
a later item fails to construct.

Tuple values use shared immutable storage. Copies with the same runtime type
retain storage. A copy into a compatible different runtime type constructs each
item through its destination runtime type. Replacing a time series value
preserves owning copies of the previous tuple. Item offsets belong to the
runtime type. Tuples and hierarchical scalars reuse the scalar allocation pool
for nested ownership.

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

`bundle<Schema>(arguments...)` constructs a `ScalarValue<Bundle<Schema>>` from
scalar sources, scalar handles, tuple sources, and contiguous ranges for list
fields. Positional arguments follow inherited logical field order. Named
arguments use `_n`; a positional argument cannot follow a named argument.
Optional fields may be omitted. An overload taking `bg::allocator&` as its
first argument places owned storage under that allocator. Construction uses
the engine's composed hierarchical runtime type. Extension runtime types
provide their own construction APIs. Fields are constructed directly in final
storage, and source arguments are borrowed only until `bundle()` returns. Pass
views when the source container should not be copied:

```cpp
template <template <typename, auto...> typename Field>
struct HeaderBatch {
    Field<List<Tuple<string, string>>> headers;
};

const auto headers = std::array{std::pair{std::string{"x-trace"}, std::string{"first"}}};
const auto value = bundle<HeaderBatch>(memory, "headers"_n = std::span{headers});
```

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

The factory that creates the provider's semantic descriptor records a callback
with `ScalarType::set_default_runtime_type_factory()`. The callback takes the
semantic descriptor and returns the trait-defined default runtime. This makes
the default available to consumers that retain only the semantic type,
including empty literal inputs. Registration stores the callback without
constructing the runtime. A declaration that reuses a semantic descriptor with
a recorded callback retains that callback.

`ScalarType::default_runtime_type_factory()` provides default runtime dispatch.
Built-in scalar descriptors provide their factory directly. Structural scalar
descriptors return a recorded trait factory when present; otherwise they return
a factory for composed storage that resolves component defaults recursively.
Creating composed storage does not prevent a later C++ declaration from
recording its trait factory during setup. Default resolution invokes this
callback and validates the returned runtime's semantic type and interface.

`scalar_declaration_traits<T>` controls whether graph signatures accept the
declaration as a collection, an implicit scalar argument, or a time series
value. Its `generic` flag identifies declarations containing type variables.
Providers enumerate those variables through
`declaration_variable_traits<T>::for_each`. The defaults describe a concrete
ordinary leaf scalar.

`scalar_storage_traits<T>::supports_alternative_layouts` states whether typed
reads must interpret different stored layouts. `false` requires every runtime
used for a typed `T` read to expose the trait's `stored_type` layout. Runtime
objects can differ while sharing that layout. BG's ordinary fixed-layout traits
declare `false`. A provider specialization that omits the member is treated as
supporting alternative layouts.

Fixed-layout typed scalar and `TS<T>` reads call the trait's `read()` directly.
Scalar reads still interpret their descriptor mode: a source view contains the
C++ read value, an inline value contains value bytes, and a shared descriptor
contains its representation's handle. Generic and hierarchical views retain
the actual runtime type.

Providers can supply these storage-trait operations:

| Operation | Contract |
| --- | --- |
| `runtime_type(const ScalarType&)` | Select the runtime for a resolved semantic type. |
| `read_value(const ScalarData&)` | Read an actual runtime descriptor as the provider's C++ read value. |
| `runtime_type_for_copy(const ScalarData&)` | Select the destination runtime for a typed owning copy. |
| `borrow_value(const value_type&)` | Return a descriptor borrowing the read value's runtime and payload. |

These operations are extension-facing APIs. Node code uses the typed value
interfaces. A borrowed descriptor retains no ownership; its payload must remain
valid for the operation that consumes it.

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
interpret the payload using its mode. For a non-reference read without a trait
hook, an alternative runtime constructs the provider's declared C++ read value
through `read_value(mode, data_word, destination)` in aligned, uninitialized
storage. BG destroys that value after use, including when moving it throws.

A provider can specialize how a typed view reads runtime representations by
adding `static value_type read_value(const details::ScalarData &source)` to
`scalar_storage_traits<T>`. The hook returns exactly the trait's `value_type`
and interprets the source runtime, payload, and mode. It handles the canonical
runtime directly when that runtime delegates reads back through the trait; it
must not call `source.runtime_type().read_value(...)` unconditionally in that
case. These trait hooks are extension APIs; node code uses `Scalar` and
`ScalarValue`.

A provider supporting alternative layouts with a reference-valued `value_type`
must supply this hook. It returns a reference to an existing source-owned object.
BG checks the hook requirement when compiling typed reads. Fixed-layout
reference reads use the trait's `read()` directly.

A provider can also add
`static const ScalarRuntimeType &runtime_type_for_copy(const ScalarData &source)`.
This selects the destination runtime when `ScalarValue<T>` copies a source.
The trait's selection takes priority over the runtime's selection, including
when the trait returns the source runtime. When the trait hook is absent,
typed scalar copies keep their existing canonical or actual-runtime copy policy.

`ScalarRuntimeType::runtime_type_for_copy(const ScalarData&) const` selects the
destination runtime for an erased owning copy. Its default returns the source
runtime. Providers override it when the owning representation differs from the
borrowed representation. The runtime selector preserves the source's actual
semantic type, including its actual schema.

BG selects the destination once. If it is the source runtime, BG can use the
existing inline-copy or shared-retain operations. Otherwise, BG allocates using
the destination's size and alignment and calls its `construct_from_input()`;
the destination must accept the source representation. A constructed owning
value records the selected destination runtime. Shared retain records the actual
runtime returned by `retain_shared()`. Both selectors are extension-facing APIs.
Node code constructs `ScalarValue` through its normal constructors.

An owning copy of mutable borrowed data must capture independent value storage.
An immutable representation can retain shared storage. Selecting a runtime does
not retain the source or perform the copy; construction supplies the ownership
required by the selected representation.

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

A runtime type for a tuple, list, set, or dictionary semantic type derives from
the matching runtime interface. Its component runtime types correspond
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

Use `Scalar::visit()` for structural tuple, list, set, dictionary, compound, and
hierarchical handling. An operation handles built-in atomic types through typed
`Scalar<T>` callbacks. It performs `ScalarDispatch` lookup in the `Scalar<>`
extension catch-all callback. Pass the operation object to a registered handler
when the handler must recurse.

Complete registration before concurrent lookup. `ScalarDispatch::add()`
rejects a duplicate exact registration.

`ScalarBufferOperations` is the shared optional operation set for scalar values
stored in buffers. It registers independent project, write, read, and discard handlers
through `ScalarDispatch<ScalarBufferOperations>::instance()`. Scalar extensions
can use this registry for any buffer-backed facility; retained tick storage is
one consumer. Write handlers receive `ScalarBufferWriteContext`, which exposes
typed emplacement and byte writes without exposing the underlying buffer
writer. A runtime registration can provide only the actions that depend on
that representation. A caller can use `find_exact()` to select each action
independently and fall back to the semantic registration when that action is
absent.

Read handlers advance the reader without moving or destroying encoded objects.
They must support repeated reads through reader forks. Discard handlers destroy
the encoded objects.
Read handlers advance past the scalar before invoking the consumer, which can
read a nested payload from the same reader.

Bounded reservations require a projection handler on the same registration as
the selected write handler. The handler receives the scalar value and a
`ScalarBufferProjectionContext`. It counts each raw byte write with
`write_bytes(size)` and each typed emplacement with `emplace<T>()`, in encoding
order. The context includes typed alignment and checks size overflow. Projection
must produce the exact size of the write handler's encoding at the supplied
position. It must not construct payload objects or change the scalar value.
It can be called more than once per write. Sequential buffer writers do not
require a projection handler.
