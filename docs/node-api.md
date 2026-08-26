# Node API

## Purpose

The node API defines a node signature and its evaluation function together. It
supports plain value access, complete time series access, and tracked output
modification.

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
resolves `lhs` as a time series input and `rhs` as a scalar. The resolved node
signature records these categories.

A parameter of type `T` is required when it resolves to a time series input. A
parameter of type `std::optional<T>` is optional when it resolves to a time
series input. The function receives `std::nullopt` while that input is invalid.
When the argument is a scalar `T`, the function receives an engaged value.
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

## Complete time series access

`In<TS_T, Name, Activity, Validity>` declares an input and provides read-only
access to its time series. Wiring requires its graph-code argument to be a time
series:

```cpp
inline const auto value_or_zero = bg::node(
    "value_or_zero",
    [](bg::In<bg::TS<float>,
              "value",
              bg::active,
              bg::optional> value) noexcept {
        return value.valid() ? value.value() : 0.0F;
    });
```

Activity defaults to `active`. Validity defaults to `required`. The common form
is therefore:

```cpp
bg::In<bg::TS<float>, "value">
```

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

The invocation entry point constructs each `In` value for one function call.
Its name, activity, and validity constraint are compile-time data and use no
runtime storage. Its last-updated time includes
[sampling](time-series-references.md#subscriptions) when the input is bound
through a reference.

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

`_n` creates a compile-time named argument. Unknown and repeated names are
compile errors.

## Evaluation context

A node function can request `EvaluationContext&` as its first parameter. The
engine injects it independently of graph code.

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
using Prices = bg::TSD<std::string, bg::TS<float>>;

inline const auto copy_prices = bg::node(
    "copy_prices",
    [](bg::In<Prices, "source"> source,
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
