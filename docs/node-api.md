# Node API

## Purpose

The node API defines a signature and its evaluation function together. It
supports plain value access, complete time series access, and tracked output
modification.

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
- uses plain C++ fields for scalar parameters
- can contain `State<T>` fields and at most one `Out<>` field
- omits names from `In<>` because field names supply them
- defines `evaluate()` as a `const` function with a `void` return type
- contains at most 32 fields

The aggregate is a read-only view created for one invocation. `State<T>` and
`Out<>` modify the runtime objects that they reference. Activity and validity
are properties of each `In<>` type.

Calling a node template inside a `WiringContext` creates a wired node and
returns its output as a `WiringPoint`. A call returns `void` when the node has
no output. The [graph wiring design](graph-wiring.md) defines ownership and
nested contexts.

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

The initial implementation requires `T` to be nothrow default constructible and
trivially copyable. Its default value must remain valid after byte copying to a
different address. These requirements preserve the runtime graph construction
defined by the [node layout](node-layout.md#packed-runtime-storage).

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

By default, invalidation of a required input invalidates the node output. The
evaluation function runs only with valid required inputs.

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

using Prices = bg::TSD<std::string, bg::TS<float>>;

inline const auto copy_prices = bg::node(
    "copy_prices",
    [](bg::In<Prices, "source"_n> source,
       bg::Out<Prices> output) noexcept {
        for (const auto& key : source.added()) {
            output.set(key, source[key].value());
        }

        for (const auto& key : source.modified()) {
            output.set(key, source[key].value());
        }

        for (const auto& key : source.removed()) {
            output.remove(key);
        }
    });
```

`Out<TS_T>` provides the mutation interface for `TS_T`. An accepted mutation
updates the affected timestamps and collection change records. After the
function returns, the engine uses this information to schedule subscribers. If
the function records no modification, the output remains unchanged.

`Out<TS<T>>::set(value)` assigns the value and sets its last updated time to the
current evaluation time. `invalidate()` applies the invalidation defined by the
[validity design](time-series-validity.md).

`set_value()` accepts a compatible scalar view. It assigns the value before it
sets the last updated time.

A node function has either a returned output or one `Out<TS_T>` parameter.
`std::optional` controls node API access or publication. It is not a graph
scalar type, so `TS<std::optional<T>>` is invalid.

### Collection item views

An `Out<>` collection operation returns a fixed-size `Out<>` item view. The
view data is allocated from stack-like storage provided through
`EvaluationContext`. It contains:

- a pointer to the item time series
- a pointer to the view of its containing collection
- the item index in that collection

The returned `Out<>` is a move-only handle to this data. Its destructor releases
the allocation when it is at the top of the storage. The engine releases all
remaining allocations when the node function returns or throws. An item view
must not outlive its node invocation.

An accepted item mutation follows the pointers to the containing collection
views and records each affected item before the view is released.
