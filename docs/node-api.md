# Node API

## Purpose

The node API defines a signature and the functions that implement a node. It supports plain value access, complete
time series access, and tracked output modification.

## Aggregate class declaration

A node can be declared as an aggregate class. The class name supplies the node
name. Each field declares a time series input, scalar parameter, private state,
or output. Parameter field names supply their signature names.

```cpp
struct add_state {
    int evaluation_count{};
};

struct add {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    float scale;
    bg::State<add_state> state;
    bg::Out<bg::TS<float>> output;

    void evaluate() const noexcept {
        ++state->evaluation_count;
        output.set((lhs.value() + rhs.value()) * scale);
    }
};

inline const auto add_node = bg::node<add>();
```

`node()` creates the node template. `NodeTemplate` is its polymorphic interface.
An aggregate node class:

- uses `In<>` fields for time series inputs
- uses plain C++ fields or Scalar<T> for scalar parameters
- can contain `State<T>` fields and at most one `Out<>` field
- defines `evaluate()` or `start()` as a `const` function with a `void` return type
- contains at most 32 fields

The aggregate is a read-only view created for one invocation. `State<T>` and
`Out<>` modify the runtime objects that they reference. Activity and validity
are properties of each `In<>` type.

`evaluate()` can be omitted when a node performs no evaluation. The compiled node then has a null evaluation function.

Calling a node template inside a `WiringContext` creates a wired node and
returns its output as a `WiringPoint`. A call returns `void` when the node has
no output. The [graph wiring design](graph-wiring.md) defines ownership and
nested contexts.

## Runtime-signature declaration

A definition object can provide its signature as runtime data. Use this form
when the parameter count or parameter types cannot be encoded in one C++ type.
`node()` creates a node template for the definition.

The definition can declare two C++ types:

- `compiled_data_type` is immutable, belongs to the compiled graph, and is
  shared by its runtime graphs
- `state_type` is mutable and belongs to one runtime graph

The definition constructs its compiled data during graph compilation. Initial
state follows the byte-copy requirement in [Private state](#private-state).
Either type can be omitted.

The definition declares `evaluate()` or `start()`. Each function receives a
`NodeInvocation<CompiledData, State>` by value. The same value is available to declared stop and invalidation
functions. It provides `context()`, `output()`, and the resolved parameters. `output()` is
available when the signature has an output. The invocation also provides
`compiled_data()` and `state()` when the definition declares those types.

`inputs()` enumerates time series inputs in signature order. `scalars()`
enumerates scalar parameters in signature order. Every entry provides
`parameter_index()`, `parameter_name()`, and `parameter_type()`. An input entry
provides a generic `In<TS_T>` view. A scalar entry provides `has_value()` and a
generic `Scalar<>` view. An absent optional scalar remains in the range with
`has_value() == false`.

The ranges are lazy and allocate no storage. Views are returned by value and
are valid for one node invocation. They use the generic view visitor and
conversion APIs.

The node framework alone accesses compiled parameter bindings and runtime
storage.

## Plain C++ parameters

A plain C++ function declares its parameter names separately:

```cpp
inline const auto add = bg::node(
    "add",
    bg::parameters("lhs", "rhs"),
    [](float lhs, float rhs) noexcept {
        return lhs + rhs;
    });
```

Wiring classifies each parameter from its graph-code argument:

```cpp
auto series_sum = add(lhs_series, rhs_series);
auto shifted = add(lhs_series, 2.0F);
```

The first call resolves both parameters as time series inputs. The second call
resolves `lhs` as a time series input and `rhs` as a scalar. The resolved
signature records these categories.

A parameter of type `T` is required when it resolves to a time series input. A
parameter of type `std::optional<T>` accepts an absent scalar or an invalid time
series input. The resolved category determines the source of absence.
`std::optional<T>` is node API syntax rather than a graph scalar type.

Parameter descriptions can select the activity used when the parameter resolves
to a time series input. Inputs are active by default.

```cpp
bg::parameters(
    bg::parameter("trigger", bg::active),
    bg::parameter("value", bg::passive))
```

Only resolved time series inputs create graph dependencies and subscriptions or
participate in validity gating. Scalars remain immutable shared wired-node data.
A node resolved with only scalars evaluates once when its runtime graph starts.

`std::optional<T>` owns its value, so passing a valid time series input can copy
`T`. Use `In<TS<T>, ...>` to access an expensive value by reference.

## Scalar parameters

An aggregate plain C++ field is an immutable scalar parameter. Wiring requires
a scalar of the field's registered graph type.

`Scalar<V>` requests a read-only view when `V` is a scalar type variable.
`Scalar<List<V>>` provides the corresponding view of a scalar list. The
[scalar API](scalar-api.md) defines their operations and lifetimes.

An aggregate `std::optional<T>` field accepts `T` or `std::optional<T>`. A
disengaged `std::optional<T>` supplies no value.

## Private state

`State<T>` gives node code mutable state that persists between invocations.
Each runtime graph owns one `T` object for each `State<T>` field.
`State<T>::operator->` and `operator*` access that object.

A plain function can request `State<T>` as a parameter. The engine supplies it
independently of graph code.

An ordinary C++ state type must be nothrow default constructible and trivially
copyable. Scalar collection state does not retain a memory resource. It uses the
node allocation context and exposes a mutable scalar view.

A collection state declaration can contain scalar type variables. Wiring
resolves these variables with the node signature. Graph compilation selects a
registered runtime type when one matches the resolved type. Otherwise, it
composes a runtime type from the resolved component runtime types.

Each initial state must remain valid after byte copying to a different address.
This is required for the runtime graph construction defined by the
[node layout](node-layout.md#packed-runtime-storage).

## Complete time series access

`In<TS_T, Properties...>` declares an input and provides read-only access to its
time series. Wiring requires its graph-code argument to be a time series:

```cpp
using namespace bg::literals;

inline const auto value_or_zero = bg::node(
    "value_or_zero",
    [](bg::In<bg::TS<float>,
              bg::optional,
              "value"_n> value) noexcept {
        return value.valid() ? value.value() : 0.0F;
    });
```

Activity defaults to `active`. Validity defaults to `required`. The common form
is therefore:

```cpp
bg::In<bg::TS<float>, "value"_n>
```

Property order has no meaning. A property can be omitted without spelling the
properties before it. Repeating a property category is a compile error.

All time series types provide `valid()` and `last_updated()`. `TS<T>::value()`
returns `const T&`. `value()` and `last_updated()` require a valid time series.
Collection time series provide their type-specific change queries.

A time series dictionary input can request the `valid_items` guarantee. Its
view exposes only keys whose item time series are valid. It reports:

- `added` when a key starts identifying a valid item
- `modified` when a visible item updates and remains valid
- `removed` when a visible key is removed or its item becomes invalid

The view derives these changes from the source dictionary. An equal-type
binding selects this view directly.

An `In` value is valid for one function call. Its last-updated time includes
[sampling](time-series-references.md#subscriptions) when the input is bound
through a reference.

Fully specified and generic inputs expose the same common interface to helper
functions and visitors. A fully specified input also provides direct typed
operations such as `value()`. The arrow operator provides const access to the
common interface.

`value_view()` returns `Scalar<>` for `TS_T`, `Scalar<SCALAR>` for
`TS<SCALAR>`, and `Scalar<T>` for `TS<T>`.

## Required input invalidation

By default, invalidation of a required input invalidates an invalidatable node
output. A TSS or TSD output preserves its value. The evaluation function runs
only with valid required inputs.

A node can preserve its output by setting the `on_invalidation` named argument:

```cpp
using namespace bg::literals;

inline const auto hold = bg::node(
    "hold",
    "on_invalidation"_n = bg::preserve_output,
    [](float value) noexcept {
        return value;
    });
```

The named argument can instead provide a function:

```cpp
inline const auto fallback = bg::node(
    "fallback",
    "on_invalidation"_n = []() noexcept {
        return 0.0F;
    },
    [](float value) noexcept {
        return value;
    });
```

This function uses the same returned-output and `Out<>` rules as evaluation. It
can request `EvaluationContext`, `In<>` views, scalars, private state, and the
output. Producing no output change preserves the current output.

An aggregate node declares `void on_invalidation() const` for this function. It
can instead set `static constexpr auto on_invalidation = bg::preserve_output`.

`_n` creates a compile-time named argument. Unknown and repeated names are
compile errors.

## Evaluation context

A node function can request `EvaluationContext*` as its first parameter. The
engine injects it independently of graph code.

An aggregate node requests it as the parameter of `evaluate()`, `start()`,
`stop()`, or `on_invalidation()`.

The [evaluation design](evaluation.md) defines its services, lifecycle use,
cycle callbacks, and failure behavior.

## Returned outputs

A function that returns `T` requests an update of its `TS<T>` output on every
invocation. The [type system](type-system.md#runtime-enforcement) defines how the
resolved output guarantee publishes the update.

A function can return `std::optional<T>` to control whether it updates its
output:

```cpp
[](float value) -> std::optional<float> {
    if (value < 0.0F) {
        return std::nullopt;
    }

    return value;
}
```

An engaged result updates the output. `std::nullopt` keeps the current output
value, validity, and last-updated time.

## Output parameters

A function uses `Out<TS_T>` when it must apply multiple or type-specific output
modifications. The `Out` parameter is last, and the function returns `void`.

```cpp
using namespace bg::literals;

using Prices = bg::TSD<bg::string, bg::TS<float>>;

inline const auto copy_prices = bg::node(
    "copy_prices",
    [](bg::In<Prices, "source"_n> source,
       bg::Out<Prices> output) noexcept {
        source.for_each_added(
            [&](const auto& key) { output[key].set(source[key].value()); });
        source.for_each_modified(
            [&](const auto& key) { output[key].set(source[key].value()); });
        source.for_each_removed([&](const auto& key) { output.erase(key); });
    });
```

`Out<TS_T>` provides access to the resolved output interface. Each specific
output type provides its mutation operations.

`Out<TS<T>>::set(value)` assigns the value and sets its last updated time to the
current evaluation time. Invalidatable output types provide `invalidate()` with
the behavior defined by the [validity design](time-series-validity.md).

`Out<TSS<T>>::clear()` and `Out<TSD<K, TS_T>>::clear()` remove all entries and
produce a valid empty collection.

`Out<TS<T>>::set_value()` accepts a compatible scalar view. It assigns the value
before it sets the last updated time.

A node function has either a returned output or one `Out<TS_T>` parameter.
`std::optional` controls node API access or publication. It is not a graph
scalar type, so `TS<std::optional<T>>` is invalid.

### Collection item views

`Out<>` is a one-word, move-only view. Root and collection item outputs use the
same type. It points to mutation state owned by the current node invocation.
The engine reuses mutation state as temporary views are destroyed and releases
the remaining state when the node function returns or throws. An `Out<>` value
is valid for one node invocation.

A TSD item view is valid until its item is erased or the node invocation ends.

An accepted item mutation records each affected item in its containing
collection. After the function returns, the runtime follows the collection change
information and the compiled item subscriptions to schedule subscribers.
