# Type system

Status: working design. Open choices are listed at the end.

## Purpose

The type system gives wiring, graph compilation, and node code one description
of every scalar and time series type. It defines type identity, compatibility,
generic signatures, and time series guarantees.

## Type model

Generic and resolved types use one hierarchy rooted at `Type`. Its two main
interfaces are `ScalarType` and `TimeSeriesType`. Each type exposes its structure
through the applicable interface. Matching and resolution use virtual operations
on these interfaces.

Generic and resolved instances of one type form share its semantic interface.
For example, both `TS<float, G>` and `TS<float, final>` are time series value
types. `TSD<K, V>` remains a time series dictionary type before its components
are resolved.

`is_scalar()` and `is_time_series()` report the known category. A variable over
`Type` reports neither until resolution. Every type in a resolved signature
reports exactly one.

Type objects are process-wide so modules can register types before an engine
exists. Resolved objects are immutable and canonical. Equivalent types have the
same address, so exact equality is a pointer comparison. C++ and Python
declarations use the same objects. A module that implements a registered type
must remain loaded while the type can be used.

Physical storage is separate from this hierarchy.

## Storage representations

A storage representation describes the typed objects and buffers used to store
a graph type and construct its views. Its parts can occupy separate locations.
A graph type can have several storage representations, and graph types can
share one.

Type resolution produces semantic types. Graph compilation selects storage
representations and uses them to lay out values and select input views and entry
points. A representation used in initial runtime storage provides a
position-independent initial value that supports byte-copy initialization.

## Scalar types

Scalar types separate graph identity and compatibility from C++ or Python
storage. Each registered scalar type has a fully qualified `Symbol`. A C++
type has one default graph type, while several graph types can share the
same storage type.

`EvaluationTime` is a scalar type. Graph code can use it directly and as a
component of another scalar type, such as `List<EvaluationTime>`.

`Opaque<T>` carries private C++ data as a wiring-time scalar. It has exact
compatibility and provides no equality or hashing.

A compound scalar has an ordered field schema and exact compatibility.

A scalar list has one scalar item type. `List<SCALAR>` accepts any resolved
scalar item type. Each list is homogeneous.

The [scalar API](scalar-api.md) defines C++ access and ownership.

A hierarchical scalar adds explicit graph inheritance. This inheritance is
independent of C++ and Python inheritance. Direct bases are ordered and use the
C3 method resolution order.

Each hierarchical scalar records the fields it adds. A field inherited through
more than one path appears once. A derived type must resolve conflicting
inherited fields by declaring a compatible, narrower field type.

Hierarchical scalar values are immutable and stored by handle. This keeps their
time series value field pointer-sized while allowing different derived layouts.
The handle identifies the actual type. A value declared as `Base` can hold
`Base` or a derived type, and an update replaces the handle. Compound scalar
storage can be inline.

## Covariance

Covariance lets an input accept hierarchical values without creating another
time series representation. If `Derived` inherits from `Base`, compatibility
applies recursively:

```text
Derived <: Base
List<Derived> <: List<Base>
Dictionary<Derived, V> <: Dictionary<Base, V>
TS<Derived> <: TS<Base>
```

A covariant binding retains the source's declared component type. A container
declared for `Base` accepts `Base` and its derived types. A container declared
for `Derived` accepts `Derived` and its derived types. Dictionary lookup and set
membership treat a value outside the declared constraint as absent.

Graph compilation implements a covariant time series binding with a direct
input view.

## Time series types

Each time series form exposes its components through a `TimeSeriesType`
interface. For example, `TSD<K, V, G>` exposes its key type, item time series
type, and guarantee.

A variable scalar component provides a type-erased scalar view. A variable time
series component provides a type-erased time series view. This lets one compiled
node implementation work with any resolved component type accepted by its
signature.

### Time series bundles

A named `TSB` uses nominal compatibility through declared bundle inheritance.
Bundle inheritance uses ordered direct bases and the C3 method resolution
order. Its item inheritance and narrowing rules match hierarchical scalar field
rules.

A named `TSB` input also accepts an unnamed, automatically bound `TSB` with a
compatible item schema. Graph compilation maps the item offsets directly.

## Variables

Variables express relationships between types in one signature. Reusing a
variable requires each occurrence to resolve to the same value.

`ScalarVariable<Name>` declares a scalar type variable in C++ code. `SCALAR`
is the default declaration. Declarations with different names bind
independently. Additional template arguments constrain the accepted scalar
types, as in `ScalarVariable<"Number", float, double>`.

`EqualityComparable` and `Hashable` require the corresponding scalar
operation. A scalar variable view exposes an operation when its declaration
requires it:

```cpp
using Key = ScalarVariable<"Key", EqualityComparable, Hashable>;
```

C++ scalar types derive these properties from their value operations. A
collection scalar has a property when its item type has that property.

Each variable derives from `Variable`. `TypeVariable<Category>` also implements
its type category, so `TypeVariable<ScalarType>` can be used wherever a scalar
type is required. Size and guarantee variables derive only from `Variable`.

A type variable can restrict its value to alternative generic types:

```text
Numbers = TypeVar<ScalarType, int, float>
Value   = TypeVar<Type, float, TS<float, G>>
```

Exactly one alternative must accept the supplied type. Matching uses normal
compatibility. An unconstrained variable accepts any type in its category.

`TypeMap` holds the bindings produced for one signature resolution. It is keyed
by `const Variable*` and stores:

```cpp
std::variant<const Type*, TimeSeriesGuarantee, Size>
```

The variable object provides identity within a signature. A separate `TypeMap`
lets another signature reuse the same variable independently.

## Signature resolution

Resolution produces one concrete and internally consistent signature
before graph compilation:

1. Match supplied argument types against generic parameter types and build
   the `TypeMap`.
2. Resolve every parameter and output type from that map.

This also classifies each plain C++ function parameter as a scalar or time
series input. A plain `float` parameter can accept `float` or `TS<float, G>`;
the graph-code argument selects the category.

A node template can provide a function to resolve a variable that matching did
not bind. The function returns a resolved value and can use the `TypeMap` and
declared wiring-time scalars. A scalar requested by the function resolves its
parameter as a scalar. Functions are evaluated on demand, with dependency
cycle detection.

Graph compilation uses the resolved signature to select direct, reference,
covariant, or representation bindings.

### Types passed as scalars

`TypeOf<T>` is a scalar whose value is a resolved graph type compatible with
`T`. Python uses `Type[T]` for the same declaration.

Wiring represents a supplied type `X` as the canonical scalar type `TypeOf<X>`.
Matching then applies the normal type rules to `T` and `X`:

- `TypeOf<V>` binds type variable `V` to `X`.
- `TypeOf<TSD<K, V>>` checks that `X` is a compatible TSD and binds `K` and `V`.

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

The default is `ordinary`. The guarantees are ordered from strongest to
weakest:

```text
const -> final -> deduplicated -> ordinary
```

The C++ enumerator for `const` is `const_`.

An input accepts a source with the same or a stronger guarantee. A guarantee
variable can declare a literal minimum. When several inputs bind the same
variable, it resolves to their weakest guarantee. A scalar contributes `const`.

An omitted input guarantee accepts and preserves the source guarantee. An
omitted output guarantee resolves to `ordinary`.

For example, a generic `add(float, float)` signature is:

```text
lhs:    TypeVar<Type, float, TS<float, G>>
rhs:    TypeVar<Type, float, TS<float, G>>
output: TS<float, G>
```

The shared `G` gives the output the weakest input guarantee. Output publication
enforces that guarantee.

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
retains its guarantee. Dereferencing and sampling preserve only guarantees
proved for the resulting input view.

### Enforcement

Graph compilation selects output publication behavior from the resolved
guarantee. A suppressed mutation keeps the current last updated time and does
not schedule subscribers. Scalar types provide equality for deduplication;
collections use their change records.

An accepted mutation records a change. Restoring the previous value later in
the same invocation does not remove that record. Collection storage enforces
effective child guarantees before it notifies subscribers.

## `valid_items`

`valid_items` lets a TSD input exclude invalid immediate items without changing
the source or output dictionary. It is a field of each generic TSD input type,
including each nested TSD.

For an equal-type binding, graph compilation selects a filtered direct view.
The view derives its visible keys and changes from the source key,
item-validity, and item-modification records.

A representation request includes `valid_items` when it changes the item type.
The node that provides representations maintains that view. A reference
representation can have a different visible key set from the source.

## Errors

A matching, variable resolution, or constraint failure throws a structured
type-resolution exception. It identifies the affected parameter or output
and the incompatible types.

## Open choices

1. C++ interfaces for variables and functional resolvers.
2. Equality rules for values such as a floating-point not-a-number.
3. Whether a Boolean `valid_items` field covers all nested TSD requirements.
4. Allocation and ownership of immutable hierarchical scalar values.
