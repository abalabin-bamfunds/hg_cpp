# Type system

## Purpose

The type system gives wiring, graph compilation, and node code one description
of every scalar and time series type. It defines type identity, compatibility,
generic signatures, and time series guarantees.

## Type model

Generic and resolved types use one hierarchy rooted at `Type`. Its main
categories are scalar, time series, and graph types. Each type exposes its
structure through the applicable interface. Matching and resolution use virtual
operations on these interfaces.

`Type::format_name(result)` appends the type's formal diagnostic name to
`result`. Built-in types format their own names and nested type arguments.
Extensions can override the same operation. The default `Type` name is `type`,
and the default `ScalarType` name is `scalar`.

`type_cast<T>()` casts between the structural categories in `TypeVisitor`.
Pointer casts return null on mismatch. Reference casts throw `std::bad_cast`.

Generic and resolved instances of one type form share its semantic interface.
For example, both `TS<float, G>` and `TS<float, final>` are time series value
types. `TSD<K, V>` remains a time series dictionary type before its components
are resolved.

`is_scalar()`, `is_time_series()`, and `is_graph()` report the known category. A
variable over `Type` reports none until resolution. Every parameter in a
resolved signature reports one supported category.

Every `Type` must be registered. Committed objects have process lifetime; failed
registration transactions discard their pending objects. Static declaration
helpers cache references to registered objects. Dynamic composition can register
unnamed types automatically. This is required to retain type pointers after a
graph or wiring template is destroyed.

Engine and extension implementations use `TypeRegistry::register_static()` for
objects with process-static storage and `TypeRegistry::instance().register_owned()`
to transfer dynamic ownership. Both return a stable typed reference. Static
registration allocates no memory; primitive accessors remain `noexcept`.
Type objects cannot be copied or moved.

`TypeRegistry::instance().get_or_create<Descriptor, Hash>(key, factory)` provides
a shared cache for ordinary canonical factories.
The factory defines the identity key, validates semantic requirements, and constructs
an unregistered descriptor. The registry owns the cache and descriptor, publishes
one canonical object per key, and removes pending entries if publication fails.
Descriptor, key, and hash types identify separate cache families.

Factories run without registry locks and can request other types. Concurrent
first requests can construct temporary duplicate candidates; every successful
caller receives the same canonical object. Factories must tolerate duplicate
construction and must not request their own key recursively. This operation
commits one descriptor; recursive scalar schemas use their
[group registration transaction](decisions/0018-atomic-recursive-semantic-registration.md).

`VariableRegistry` retains non-type metadata such as guarantee and size variables.
Type variables register through `TypeRegistry`; their variable interface shares
that object's lifetime. Independent variables keep separate identities even when
their names match. Category factories define canonical type identity and own
any group registration transaction they require.

Resolved objects are immutable and canonical. Equivalent types have the same
address, so exact equality is a pointer comparison. Modules can register types
before an engine exists. A module that implements a type must remain loaded
while the type can be used.

A C++ time series declaration exposes its semantic type through `type()`.
This constructs a neutral declaration: an omitted root guarantee is `ordinary`,
including for references. [Collection guarantees](#collection-guarantees) still
apply. Generic declarations retain their declared variables:

```cpp
TS<float>::type()
REF<TS<float>>::type()
TSD<i64, TS<float>>::type()
TS<SCALAR>::type()
```

Scalar variable declarations expose the same access, for example
`SCALAR::type()`. Resolved scalar types come from their type registry.

Runtime types define physical storage separately from this hierarchy.

A graph type contains its parameter types and optional time series result type.
Its nested variables use the surrounding signature's type map. The
[`GRAPH` declaration](operators.md#graph-arguments) can also attach a structural
constraint to its signature parameter. Structural constraints and graph
argument values are stored separately from their process-lifetime types.
Type-only signature resolution resolves the boundary types. Wiring argument
validation also checks the parameter's structural constraint against the actual
graph argument.

## Runtime types

A runtime type pairs a semantic type with a physical layout and the operations
that interpret and manage its data. Its data can occupy separate locations. A
semantic type can have several runtime types. Runtime types for different
semantic types can share an implementation and physical layout.

Type resolution produces semantic types. Graph compilation selects runtime
types and uses them to lay out values and select entry points. A runtime type
used in initial runtime storage provides a position-independent initial value
that supports byte-copy initialization.

The process owns registered and composed runtime types. Graph compilation
reuses them across graphs.

## Schemas

`Schema` is the shared field descriptor for hierarchical scalars, time series
bundles, and extension types. `SchemaType` is the category used by schema
variables. Each field records its name, semantic `Type`, and optional presence.
The owning type validates its field category and defines compatibility. Runtime
types provide storage and field access.

A schema records its name, declaration identity, base schema, ordered direct
fields, and flattened fields. A redefinition keeps the inherited field position,
requires a compatible narrower type, and preserves required presence. Field
variables resolve through the ordinary `TypeMap`.

The canonical key contains the name, declaration identity, base pointer, and
ordered direct fields. Field equality compares name, canonical type pointer,
and optionality. The factory hashes these components and returns one registered
descriptor per key. Schema entries created during recursive scalar registration
commit or roll back with that group.

A C++ declaration such as `Quote` is identified by
`std::type_index{typeid(details::schema_template<Quote>)}`. A runtime declaration
uses a unique `RuntimeSchemaId` allocated by `new_runtime_schema_id()`. Anonymous
schemas use no declaration identity. Separate declarations remain distinct when
their fields match.

Projections retain that declaration identity and record their actual field types:

| Use | Descriptor fields |
| --- | --- |
| `Bundle<Quote>` | `bid: double`, `ask: double` |
| `TSB<Quote>` | `bid: TS<double>`, `ask: TS<double>` |

These projections have distinct canonical keys. Optional presence describes an
absent field; extensions define nullable cell values through their column values.
Extensions construct descriptors through `schema(name, identity, base, fields)`
or `anonymous_schema(fields)`. These factories are engine and extension APIs.

### Shared C++ schemas

A C++ schema declares fields through a supplied field template. A schema can
declare one base with `schema_extends<Base, Field>`. A derived schema can add
fields or redefine an inherited field. `bundle_extends` is a compatibility alias
for `schema_extends`.

`details::schema_reflection<Schema, ReflectionField>` provides engine and
extension access to C++ schemas. Logical fields retain base order; a redefined
field replaces the inherited field at its logical index. `field<Index>` and
`field_name<Index>()` describe that order. `make<Field>(factory)` constructs the
complete aggregate directly, including hidden base fields. The factory's
`make<ReflectedField>(logical_index)` returns each field initializer.

`fields(schema)` returns a tuple of references to the logical fields and
preserves constness. The reflected field descriptor supplies
`rebind<Field>`. Actual field wrappers support access by adding an implicit
constructor taking `details::FieldProbe&`:

```cpp
Field(details::FieldProbe &probe) noexcept { probe.record(this); }
```

This constructor leaves the field valid and safely destructible without a
usable wrapped value. `field_offsets<Field>()` captures offsets from a local
`Schema<Field>` object and caches them once per schema and field wrapper.
After cache initialization, obtaining a tuple reads the cache once and
constructs no schema object.

## Scalar types

Scalar types separate graph identity and compatibility from C++ storage. Each
registered scalar type has a fully qualified `Symbol`. A C++ type has one
default graph type, while several graph types can share the same storage type.

A concrete time series declaration uses a supported scalar declaration. Raw
C++ integer values canonicalize to `i64` at construction and wiring boundaries,
so time series signatures use `i64` rather than `int`.

`EvaluationTime` is a scalar type. Graph code can use it directly and as a
component of another scalar type, such as `List<EvaluationTime>`.

`Opaque<T>` carries private C++ data as a wiring-time scalar. It has exact
compatibility and provides no equality or hashing.

A scalar list has one scalar item type. `List<SCALAR>` accepts any resolved
scalar item type. Each list is homogeneous.

A scalar set has one hashable scalar item type, written `Set<T>`. A scalar
dictionary maps hashable scalar keys to scalar values, written `Dict<K, V>`.

`STRUCT` accepts any registered compound scalar. Compound scalar compatibility
is nominal. Its fields provide structural access after type resolution; they do
not make two distinct C++ aggregate types compatible.

`HIERARCHICAL` accepts any registered hierarchical scalar. Its fields provide
structural access after type resolution. Declared base relationships determine
compatibility between concrete hierarchical scalar types.

### Hierarchical scalars

`Bundle<Schema>` declares a hierarchical scalar. Its fields are scalar
declarations. `Field<T, optional>` marks an optional field; fields are required
by default. Each schema has nominal identity, and a base type accepts its
declared derived types.

`Bundle<SCHEMA>` captures the supplied hierarchical scalar's semantic schema. Resolving
the captured schema returns its canonical hierarchical scalar type, including
the existing type for a C++ declaration. Runtime-defined schemas use the same
type factory and composed storage.

`HierarchicalScalarType` is the engine and extension interface for this family.
`GenericHierarchicalScalarType` holds the unresolved schema;
`ResolvedHierarchicalScalarType` provides concrete fields, inheritance, and
capabilities. `HierarchicalScalarRuntimeType` always references a resolved type.

The extension factory `hierarchical_scalar_type(schema)` returns the canonical
resolved bundle type. A C++ schema requires prior registration of its bundle
type; normal access through `cpp_scalar_type<Bundle<Schema>>()` performs this
registration. An unregistered C++ schema produces a registration error.
Runtime-defined and anonymous schemas construct their bundle types directly.

Compatibility is covariant through `List` items and `Dict` values. `Set` items
and `Dict` keys remain invariant because lookup requires one hash and equality
domain.

Each resolved hierarchical scalar has a `HierarchicalScalarRuntimeType`.
The composed runtime type records field layout, optional-field presence, and
the value's actual hierarchical type. A base view of a derived value therefore
exposes the derived fields and their actual types.

The same schema can describe a hierarchical scalar and a time series bundle.
`Bundle<Schema>` requires every field to be a supported scalar declaration.
`TSB<Schema>` preserves an explicitly declared time series field and projects a
supported scalar field `T` to `TS<T>`. `TS<Bundle<Schema>>` is a time series
value whose value is the complete hierarchical scalar. An `optional` property
permits an absent `Bundle` field. In a TSB, it configures the typed input field;
the structural TSB field remains present.

Hierarchical scalar schemas can refer to themselves or each other directly and
through scalar collections. Semantic registration publishes the complete group
of types and schemas atomically. Readers observe complete field metadata.

Required direct hierarchical fields must not form a recursive cycle. Every
direct cycle must contain an optional field. A scalar collection also terminates
the direct cycle because an empty collection has a finite value.

The [scalar API](scalar-api.md) defines C++ access and ownership.

## Time series types

Each time series form exposes its components through a `TimeSeriesType`
interface. For example, `TSD<K, V, G>` exposes its key type, item time series
type, and guarantee.

`TimeSeriesType::target_type()` returns the type itself. A reference type
returns the time series type that it references.
`TimeSeriesType::dereferenced_type()` returns the type that a non-reference
consumer observes, applying the [reference guarantee limit](#reference-guarantees)
when the supplied type is a reference. TSL item inference uses this operation.

A variable scalar component provides a type-erased scalar view. A variable time
series component provides a type-erased time series view. This lets one compiled
node implementation work with any resolved component type accepted by its
signature.

The public borrowed views are `Scalar<T>` for scalars and `In<T>` for time
series. `Out<T>` provides time series mutation. The template argument controls
the available typed or generic operations; separate public view hierarchies are
not required.

External time series families use the
[provider API](time-series-providers.md) for declaration metadata, runtime
registration, and view binding.

### Time series bundles

A named `TSB` uses the shared C++ schema model and nominal compatibility through
declared schema inheritance.

A named `TSB` input also accepts an unnamed, automatically bound `TSB` with a
compatible item schema. Graph compilation maps the item offsets directly.

A TSB uses the shared `Schema` descriptor with present time series fields. A C++
schema produces typed input and output access. It can declare time series fields
explicitly:

```cpp
template<template<typename, auto...> class Field>
struct Quote {
    Field<TS<symbol>> name;
    Field<TS<float>, optional> value;
};

In<TSB<Quote>> quote;
```

It can instead declare scalar fields for use by both `Bundle` and `TSB`:

```cpp
template<template<typename, auto...> class Field>
struct Quote {
    Field<symbol> name;
    Field<double, optional> value;
};
```

Field properties configure the typed input view. Wiring and output field views
retain the field type and ignore input-only properties.

## Variables

Variables express relationships between types in one signature. Reusing a
variable requires each occurrence to resolve to the same value.

C++ variable declarations manage descriptor lifetime and registration
automatically. Use their `type()` when constructing runtime signatures.

`ScalarVariable<Name>` declares a scalar type variable in C++ code. `SCALAR`
is the default declaration. Declarations with different names map
independently. Additional template arguments constrain the accepted scalar
types, as in `ScalarVariable<"Number", float, double>`.

`TimeSeriesVariable<Name>` declares a time series type variable. `TS_T` is the
default declaration. A variable can expose its top-level guarantee as a
separate variable:

```cpp
inline constexpr auto G = guarantee_variable<"G">;
using T = TimeSeriesVariable<"T">;

In<T::with<G>> input;
```

`T` and `G` retain separate mappings. A literal guarantee in `T::with<G>`
requires that guarantee or a stronger one.

`SizeVariableDeclaration<Name>` declares a fixed time series list size
variable. `SIZE` is the default declaration. For example,
`TSL<TS_T, SIZE>` captures both the item time series type and the list size.

`SchemaVariableDeclaration<Name>` declares a schema variable.
`BundleSchemaVariableDeclaration<Name, Field>` retains the template form used by
C++ schema parameters. `SCHEMA` is the default declaration. It captures a resolved
`Schema`, including its ordered field names, types, and optional presence. Its identity is shared
across reflection field adapters. Reusing the variable requires the same
descriptor; each owning type validates the captured fields. `Bundle<SCHEMA>`
requires scalar fields; `TSB<SCHEMA>` requires present time series fields.

`schema_type<SCHEMA>` exposes the variable for resolvers and explicit type
mappings. Concrete `schema_type<Quote>` exposes the TSB projection of `Quote`.

`EqualityComparable` and `Hashable` require the corresponding scalar
operation. A scalar variable view exposes an operation when its declaration
requires it:

```cpp
using KEY = ScalarVariable<"KEY", EqualityComparable, Hashable>;
```

C++ scalar types derive these properties from their value operations. A
collection scalar has a property when its item type has that property.

Runtime variable descriptors are engine and extension APIs. Each descriptor
derives from `Variable`. `TypeVariableType<Category>` also
implements its type category, so `TypeVariableType<ScalarType>` can be used
wherever a scalar type is required. A schema variable is a
`TypeVariableType<SchemaType>`.
Size and guarantee variables derive only from `Variable`.

A root-category variable can restrict its value to alternative declarations:

```cpp
using K = ScalarVariable<"K", Hashable>;
using KEY = TypeVariable<"KEY", K, TS<K>>;
```

`Union<"key", K, TS<K>>` declares an inline union with the name `key`. Reusing
the same named union declaration in one signature selects the same concrete
type. Give independent unions different names; variables inside their
alternatives still share mappings when they use the same declaration.

A node parameter can resolve scalar alternatives, or scalar alternatives plus
one time series alternative, to a concrete parameter type. A union containing
only time series alternatives, or more than one time series alternative, is
graph-only. Graph parameters can retain materialisation options; node parameters
consume the selected concrete form.

Each alternative matches the original supplied type using normal reference
rules. Constraints and the outer-variable mapping use the observed type after
reference transparency and guarantee limits. Variables in every matching
alternative also map. Compatible overlapping alternatives combine their
mappings; conflicting mappings reject the match as ambiguous.

`Type::accepts()` checks directional compatibility without retaining variable
mappings. `Type::match()` checks the same compatibility and extends a supplied
`TypeMap` for resolution.

`TypeMap` holds the mappings produced for one signature resolution. It is keyed
by `const Variable*` and stores:

```cpp
std::variant<const Type*, TimeSeriesGuarantee, Size>
```

The variable declaration provides its identity. Each resolution operation uses
a separate `TypeMap`, so the same declaration can resolve independently in
different signatures.

## Signature resolution

Resolution produces one concrete and internally consistent signature
before graph compilation:

1. Match supplied argument types against generic parameter types and build
   the `TypeMap`.
2. Resolve every parameter and output type from that map.

A wiring template call can seed the `TypeMap`. Argument matching must agree
with each seeded map entry. This resolves variables that appear only in outputs.

An explicit type argument seeds this map before signature resolution. A wiring
construct can declare the order of its positional type parameters. Named type
mappings do not require that declaration:

```cpp
using RESULT = bg::TimeSeriesVariable<"RESULT">;

auto source = bg::node<RESULT>([](bg::Out<RESULT>) {});

source[bg::type<bg::TS<bg::i64>>]();
source[bg::type<RESULT> = bg::type<bg::TS<bg::i64>>]();
```

Positional arguments follow only the declared order. Wiring rejects an
unresolved explicit value, an incompatible value, or a value that conflicts
with another mapping or a supplied argument. Indexing a wiring construct
does not change the construct. A proxy over an lvalue wiring construct is
reusable. A proxy that owns a temporary wiring construct is consumed by its
wiring call.

Reference matching is transparent at every level of a type. A non-reference
parameter matches the target with the
[reference guarantee limit](#reference-guarantees). A reference parameter
matches the declared target and separately matches its own guarantee. A direct
source contributes a `const` reference guarantee because its generated reference
never changes target.

Resolution retains the original argument type for graph compilation. This lets
compilation preserve, create, or dereference a reference after type variables
have been resolved from its target.

This also classifies each plain C++ node parameter as a scalar or time
series input. A plain `float` parameter can accept `float` or `TS<float, G>`;
the graph-code argument selects the category.

A node template can provide a function to resolve a variable that matching did
not map. The function returns a resolved value and can use the `TypeMap` and
declared wiring-time scalars. A scalar requested by the function resolves its
parameter as a scalar. Functions are evaluated on demand, with dependency
cycle detection.

[Operator overload resolution](operators.md) applies these rules independently
to each candidate before comparing applicable signatures.

Graph compilation uses the resolved signature to select direct, reference, or
representation bindings.

### Types passed as scalars

`TypeOf<T>` is a scalar whose value is a resolved graph type compatible with
`T`.

Wiring represents a supplied type `X` as the canonical scalar type `TypeOf<X>`.
Matching then applies the normal type rules to `T` and `X`:

- `TypeOf<V>` maps type variable `V` to `X`.
- `TypeOf<TSD<K, V>>` checks that `X` is a compatible TSD and maps `K` and `V`.

The resolved parameter type retains `X`. Parameter packing can therefore pass
its type pointer without storing another scalar value in the wired node.

## Time series guarantees

Guarantees let wiring prove update behavior and let graph compilation select
the required publication checks. Every resolved time series type has one value
from the closed `TimeSeriesGuarantee` set:

| Guarantee | Constraint |
| --- | --- |
| `ordinary` | Any number of value updates |
| `deduplicated` | Suppress a requested value equal to the current value |
| `final` | Publish at most one update |
| `const` | Publish one update at the time series start time |

The guarantees are ordered from strongest to weakest:

```text
const -> final -> deduplicated -> ordinary
```

The C++ enumerator for `const` is `const_`.

An input accepts a source with the same or a stronger guarantee. A guarantee
variable can declare a literal minimum. When several inputs map the same
variable, it resolves to their weakest guarantee. A scalar contributes `const`.

Compatible time series that differ only in their top-level guarantee have a
common type with the weakest guarantee. This lets one output represent several
alternative producers without discarding their shared time series structure.

An omitted input guarantee accepts and preserves the source guarantee. An
omitted output guarantee resolves to `ordinary`, except that a `REF` output
resolves to `deduplicated`. An explicit `ordinary` guarantee disables this
default.

Wiring-point annotation checks use normal type compatibility, including
contained time series. An omitted reference guarantee accepts the supplied
guarantee; an explicit guarantee remains a minimum requirement. The annotation
retains the wiring point's actual type.

Output construction applies defaults to the output and its contained time
series. A reference target is a neutral declaration, not an output owned by
that reference. For example, `Out<TSD<K, REF<V>>>` defaults its item references
to `deduplicated`; `Out<REF<TSD<K, REF<V>>>>` defaults only the root reference.

For example, a plain callable `add(double, double)` captures each input guarantee
independently and infers its output guarantee:

```text
lhs:    TypeVar<Type, double, TS<double, G1>>
rhs:    TypeVar<Type, double, TS<double, G2>>
output: TS<double, weakest observed input guarantee>
```

Explicit declarations can share a guarantee variable between inputs and output
to propagate the weakest input guarantee. Output publication enforces the
resolved guarantee.

The start time is when a time series becomes observable: the first outer cycle
for a fixed graph output, the inner graph start cycle for an inner graph output,
or the creation cycle for a dynamic collection item. The
[validity design](time-series-validity.md#invalidation-and-revalidation) defines
how guarantees constrain invalidation.

### Guarantee propagation

A node template can calculate its output guarantee from resolved inputs and
scheduling behavior. The rule must hold for every implementation selected by
the template.

- Lifting a scalar into a time series produces `const`.
- An identity operation preserves its input guarantee.
- An output that can update only at start is `const`.
- An output that can update at most once is `final`.
- A value transformation preserves `deduplicated` when it preserves inequality
  or the output enforces deduplication.

The calculation includes input activity, validity constraints, and every way
the node can schedule evaluation.

### Callable output inference

A C++ callable returning a plain scalar infers the weakest observed input
guarantee when every time series input is `required`, scalar values are
required, activity is static, and evaluation is input-driven. Scalars
contribute `const`. Passive inputs also contribute their observed guarantee.
Reference inputs follow normal [reference matching](#signature-resolution).

Scalar-only calls use default first-cycle scheduling and infer `const`.
Disabling that scheduling gives an ordinary output. With time series inputs,
an inferred `const` output requires an active input to schedule its first
update; otherwise the output is ordinary.

Optional inputs or returns, dynamic activity, and independent startup
scheduling with time series inputs retain an ordinary output. An explicit
`Out<>` supplies its declared guarantee. Aggregate, runtime-signature, and
Python node declarations retain their output rules.

If inference selects `deduplicated` but the result scalar lacks equality,
the output is ordinary. An explicit deduplicated output requires equality.

### Collection guarantees

A collection guarantee applies recursively to its contained time series. A
contained time series keeps a stronger guarantee that it declares explicitly.
Resolved collection types store these effective guarantees.

```text
TSD<K, TS<T, ordinary>, deduplicated>
    -> items are TS<T, deduplicated>

TSD<K, TS<T, const>, final>
    -> items remain TS<T, const>
```

The collection also retains its own guarantee. For example,
`TSD<K, TS<T, final>, ordinary>` can change over time while each item updates at
most once. A `final` collection prevents later contained updates. A `const`
collection has its complete observable value at its start time.

### Reference guarantees

In `REF<TS_T, G>`, `G` applies to changes of the reference. The target `TS_T`
retains its guarantee. When a non-reference parameter receives a reference,
type resolution limits the guarantees exposed by the dereferenced input:

| Reference guarantee | Limit on exposed guarantees |
| --- | --- |
| `ordinary` or `deduplicated` | `ordinary` |
| `final` | `final` |
| `const` | `const` |

Each exposed guarantee becomes the weaker of its target guarantee and this
limit. The limit applies recursively to collection items. At a contained
explicit `REF`, it limits the reference's guarantee and preserves its target
type. Matching an explicit reference parameter keeps the reference and target
guarantees separate. Direct non-reference arguments keep their guarantees.

Matching retains the supplied child types until each parameter's category is
known. For example, an ordinary reference to `TSD<K, TS<T, final>>`
can supply `TSD<K, REF<TS<T, final>, ordinary>>`. Its generated item references
can change, but each target still updates at most once.

`Type::match()` handles reference compatibility. Engine and extension types
implement type-specific matching through `do_match()`. Its three-argument
overload receives the inherited reference limit; the default implementation
calls the two-argument overload with a canonical limited type. Collection
implementations pass the original child types and limit to `Type::match()`.

For example, an ordinary reference to a final fixed TSL exposes ordinary items.
Changing the reference can sample a different final list and publish another
item value. A deduplicated reference can also select distinct targets with equal
values: reference deduplication compares references, not sampled target values.

`TimeSeriesType::with_guarantee_limit()` is the engine and extension operation
for this calculation. It returns a canonical resolved type. Leaf types use the
base implementation; collection types override it to limit their contained time
series recursively. Reference types preserve their target type. This is
required to resolve signatures from the input behavior defined by
[reference sampling](time-series-references.md#input-sampling).

### Enforcement

Graph compilation selects output publication behavior from the resolved
guarantee. A suppressed mutation keeps the current last updated time and does
not schedule subscribers. Scalar types provide equality for deduplication;
collections use their change records.

An accepted mutation schedules publication. Restoring the previous value later
in the same invocation does not cancel publication. A collection can therefore
tick with empty final change sets. Collection storage enforces effective child
guarantees before it notifies subscribers.

## Errors

A direct signature call reports a matching, variable resolution, or constraint
failure through a structured type-resolution exception. It identifies the
affected parameter or output and the incompatible types. Operator resolution
records the same information as an expected candidate rejection, as defined in
[operator diagnostics](operators.md#diagnostics).
