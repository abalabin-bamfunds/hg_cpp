# Node API

## Purpose

The node API defines a signature and the functions that implement a node. It supports plain value access, complete
time series access, and tracked output modification.

## Aggregate class declaration

A node can be declared as an aggregate class. The class name supplies the node
name. Each field declares a parameter, private state, or output. Parameter field
names supply their signature names.

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
        output.set((*lhs + *rhs) * scale);
    }
};

inline const auto add_node = bg::node<add>();
```

`node()` creates the node template. `NodeTemplate` is its polymorphic interface.
An aggregate node class:

- uses `In<>` fields for time series inputs
- uses plain C++ fields for scalar-or-time-series parameters and `Scalar<T>` for scalar-only parameters
- can contain `State<T>` fields and at most one `Out<>` field
- defines a `const evaluate()` that returns `void` or `bool`, or a `const start()` that returns `void`
- contains at most 32 fields

The aggregate is a read-only view created for one invocation. `State<T>` and
`Out<>` modify the runtime objects that they reference. Activity and validity
are properties of each `In<>` type.

`evaluate()` returns `void` when evaluation always completes. It can return
`bool` when evaluation can pause: `true` means complete and `false` means
pending. The [evaluation design](evaluation.md#pending-evaluation) defines
pending evaluation. `evaluate()` can be omitted when a node performs no
evaluation. The compiled node then has a null evaluation function. `start()`
and the other lifecycle functions return `void`.

Calling a node template inside a `WiringContext` creates a wired node and
returns its output as a `WiringPoint`. A call returns `void` when the node has
no output. The [graph wiring design](graph-wiring.md) defines ownership and
nested contexts.

Equal calls to an output-producing node reuse one wired node by default. An
aggregate or runtime-signature definition can disable this with
`static constexpr bool deduplicate_wiring = false`. A callable definition uses
`bg::deduplicate_wiring = false` as a `node()` option.

## Callable view declaration

A callable can declare the invocation views directly. This form keeps a small
node definition local to its use:

```cpp
const auto add = bg::node(
    [](bg::In<bg::TS<bg::i64>, "value"_n> value, int amount,
       bg::Out<bg::TS<bg::i64>> output) {
        output.set(*value + amount);
    });
```

Parameters can be `In<>`, plain values, `Scalar<>`, or one `Out<>`. An `In<>` name
supplies its signature name. Captures are immutable compiled node data. Use an
aggregate declaration when the node needs state or lifecycle functions.

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

The definition declares `evaluate()` or `start()`. `evaluate()` returns `void`
or the same completion `bool` as an aggregate evaluation function. Each
function receives a `NodeInvocation<CompiledData, State>` by value. The same
value is available to declared stop and invalidation functions. It provides
`context()`, `output()`, and the resolved parameters. `output()` is available
when the signature has an output. The invocation also provides
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

A callable or aggregate node can declare a parameter with its plain C++ type:

```cpp
const auto add = bg::node([](bg::i64 lhs, bg::i64 rhs) { return lhs + rhs; });
```

Wiring classifies each parameter from its graph-code argument:

```cpp
auto series_sum = add(lhs_series, rhs_series);
auto shifted = add(lhs_series, 2);
```

The first call resolves both parameters as time series inputs. The second call
resolves `lhs` as a time series input and `rhs` as a scalar. The resolved
signature records these categories.

A parameter of type `T` is required when it resolves to a time series input. A
parameter of type `std::optional<T>` accepts an absent scalar or an invalid time
series input. The resolved category determines the source of absence.
`std::optional<T>` is node API syntax rather than a graph scalar type.

Resolved time series inputs are active. Use `In<>` to select another activity or
validity constraint. Mixing plain parameters with `In<>` or `Scalar<>` does not
disable scalar-or-time-series resolution. Matching captures the incoming time
series guarantee independently for each plain parameter.

A callable scalar return declares a `TS<canonical T>` output. Eligible
callables infer its guarantee during wiring; see
[callable output inference](type-system.md#callable-output-inference).
A disengaged `std::optional<T>` return leaves the output unchanged. A callable
`bool` return publishes a Boolean value; the completion Boolean described above
applies to aggregate and runtime-signature `evaluate()` functions. A callable
with an `Out<>` parameter returns `void`.

Only resolved time series inputs create graph dependencies and subscriptions or
participate in validity gating. Scalars remain immutable shared wired-node data.
A callable node with only scalar parameters runs in the first cycle by default.
An explicit `bg::schedule_on_start = false` disables that evaluation. Aggregate
and runtime-signature nodes retain their declared scheduling behavior.

`schedule_on_start` requests one initial evaluation in the first cycle. It does
not call `start()` and does not make a node active in later cycles. An aggregate
or runtime-signature definition declares `static constexpr bool
schedule_on_start = true`. A callable definition uses
`bg::schedule_on_start = true` as a `node()` option, together with
`bg::eval = callable`. The runtime-signature form
can provide `schedule_on_start() const` or
`schedule_on_start(const ResolvedSignature &) const`. The resolved-signature
form lets the definition request initial evaluation based on the concrete input
types selected during wiring.

Plain parameters retain their declared C++ type and use its canonical graph
type for matching. Passing a time series can copy its value. Use
`In<TS<T>, ...>` to access an expensive value by reference.

An aggregate with a plain field resolved as a time series cannot declare
`start()`, `stop()`, or `finalize()`. Wiring rejects that call. Scalar-resolved
calls remain supported. `on_invalidation()` is allowed: required
time-series-resolved plain fields contain default-constructed placeholders;
optional fields are empty. The callback must not read either as a time series
value. Scalar-resolved fields contain their actual values. Required placeholder
types must be default constructible. Automatic state destruction and
dynamic-input cleanup still run.

## Scalar parameters

`Scalar<T>` restricts a parameter to a scalar argument. Scalars remain immutable
after wiring.

`Scalar<V>` requests a read-only view when `V` is a scalar type variable.
`Scalar<List<V>>` provides the corresponding view of a scalar list. The
[scalar API](scalar-api.md) defines their operations and lifetimes.

An aggregate `std::optional<T>` field also accepts a scalar `T` or
`std::optional<T>`. A disengaged scalar supplies no value.

## Private state

`State<T>` gives node code mutable state that persists between invocations.
Each runtime graph owns one `T` object for each `State<T>` field.
`State<T>::operator->` and `operator*` access that object.

A plain function can request `State<T>` as a parameter. The engine supplies it
independently of graph code.

An ordinary C++ state type must be nothrow default constructible and trivially
destructible. A trivially copyable type supports byte-copy initialization. Any
other type declares `static constexpr bool supports_byte_copy_initialization =
true` to assert that its default state supports it. Scalar collection state does
not retain an allocator. It uses the node allocation context and exposes a
mutable scalar view.

A collection state declaration can contain scalar type variables. Wiring
resolves these variables with the node signature. Graph compilation selects a
registered runtime type when one matches the resolved type. Otherwise, it
composes a runtime type from the resolved component runtime types.

Each initial state must remain valid after byte copying to a different address.
This is required for the runtime graph construction defined by the
[node layout](node-layout.md#packed-runtime-storage).

An aggregate node can define `finalize()` or `finalize(EvaluationContext*)` to
release resources retained by its state. Finalization runs when the runtime
graph destroys the state and must be `noexcept`.

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
        return value.valid() ? *value : 0.0F;
    });
```

Activity defaults to `active`. Validity defaults to `required`. The common form
is therefore:

```cpp
bg::In<bg::TS<float>, "value"_n>
```

A call can supply `opt(point)` to require an optional parameter. A `required`
or `all_valid` parameter rejects that argument during wiring. An unmarked point
can bind to any validity constraint. The [operator rules](operators.md#optional-arguments)
define how overload selection uses this distinction.

`all_valid` requires the input root and every immediate contained time series
to be valid before each scheduled evaluation attempt:

```cpp
bg::In<bg::TSL<bg::TS<bg::i64>, 2>, bg::all_valid> values;
```

It uses the same invalidation behavior as `required`. The
[validity design](time-series-validity.md#nodes) defines which items participate.

Property order has no meaning. A property can be omitted without spelling the
properties before it. Repeating a property category is a compile error.

`dynamic_activity` permits an input to change its own scheduling activity.
`make_passive()` stops updates from that input from scheduling the node.
`make_active()` resumes them. The input starts with its declared `active` or
`passive` activity. This does not change validity checks, callbacks, or
scheduling from other inputs. A change that occurs while the input is passive
does not schedule evaluation when the input becomes active later.

All time series types provide `valid()`, `all_valid()`, `became_valid()`, `became_invalid()`,
`modified()`, and `last_updated()`. `became_valid()` and `became_invalid()`
describe the latest change. They do not imply that the change occurred in the
current evaluation. `modified()` reports a valid update in the current
evaluation. `last_updated()` returns the latest change time, including an
invalidation. `TS<T>::value()` returns `const T&` and requires a valid time
series. `all_valid()` tests the root and immediate item validity without changing
the input validity constraint. Collection time series provide their type-specific change queries.

The erased `In<TS_T>` query checks the actual runtime type. Runtime-defined nodes
can pass the declared `TimeSeriesType` to `all_valid(declared_type)` to apply a
narrowed bundle schema. `NodeInvocationInput::parameter_type()` supplies the
parameter declaration.

An `In` value is valid for one function call. Its last-updated time includes
[sampling](time-series-references.md#input-sampling) when the input is bound
through a reference.

An aggregate can declare an internal `In<REF<TS_T>, dynamic_binding>` field.
The field is excluded from the node signature. Its type variables must resolve
through the public signature. `bind()` replaces its addressable input and
`unbind()` removes the binding. The engine manages subscriptions and cleanup.

Fully specified and generic inputs expose the same common interface to helper
functions and visitors. A fully specified value input also provides `value()`
and `operator*`. `operator->` provides member access when the C++ value type
supports it. Use dot notation to access the time series itself.

`value_view()` returns `Scalar<>` for `TS_T`, `Scalar<SCALAR>` for
`TS<SCALAR>`, and `Scalar<T>` for `TS<T>`.

## Required input invalidation

By default, failure of a `required` or `all_valid` constraint invalidates an
invalidatable node output. A TSS or TSD output preserves its value. The
evaluation function runs only when every input validity constraint passes.

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
invocation. The [type system](type-system.md#enforcement) defines how the
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

inline const auto copy_prices = bg::node(
    "copy_prices",
    [](bg::In<bg::TSD<bg::string, bg::TS<float>>, "values"_n> values,
       bg::Out<bg::TSD<bg::string, bg::TS<float>>> output) {
        output.copy_tick(values);
    });
```

`Out<TS_T>` provides access to the resolved output interface. Each specific
output type provides its mutation operations.

Built-in outputs use `Out<Declaration>`. A time series provider can define additional
`Out` specializations with its own options. The complete output declaration
selects its interface and default runtime representation during wiring. Consumers see
its semantic time series type. Input representations do not select output
storage. See the [provider contract](time-series-providers.md#output-declarations)
for the extension customization points.

A definition can select its output runtime type from wiring arguments. An aggregate
declares a static hook:

```cpp
static const bg::TimeSeriesRuntimeType &
output_runtime_type(const bg::WiringArguments &arguments);
```

A callable supplies `bg::output_runtime_type = callback` together with
`bg::eval = callable`. A runtime-signature definition supplies
`output_runtime_type(const WiringArguments &) const`.

BG calls the hook after selecting the overload and resolving its signature,
before recording the node call. The hook replaces output-trait runtime selection
for that call. Its result must remain alive while the graph uses it, have the
resolved output's exact semantic type, and implement the declared `Out` interface.
The hook must return the same runtime type for the same node definition and wiring
arguments. BG uses the existing node-call identity for deduplication.
BG reports a semantic type mismatch during wiring. Compilation stores the selected
runtime type and uses it for output allocation and `Out` construction, including
concrete outputs. Evaluation uses it without calling the hook again. Outputs that
use direct access to a fixed layout retain that layout requirement and direct
scalar access. Definitions without a hook use the output declaration's default
runtime selection.

The hook receives the same fixed and flattened variadic argument view as
requirements and resolvers. Use `resolved_type()`, `resolved_output_type()`, and
`scalar()` to inspect resolved types and scalar configuration.

`Out<TS<T>>::set(value)` assigns the value and sets its last updated time to the
current evaluation time. Invalidatable output types provide `invalidate()` with
the behavior defined by the [validity design](time-series-validity.md).

`Out<TSS<T>>::clear()` and `Out<TSD<K, TS_T>>::clear()` remove all entries and
produce a valid empty collection.

`Out<TS<T>>::set_value()` accepts a compatible scalar view or an owning
`ScalarValue` rvalue. The rvalue overload transfers compatible storage and
copies when the runtime representation or allocator requires conversion.
It consumes the owner even when equality suppression skips publication.
It replaces the complete scalar value before it sets the last updated time.
Owning scalar copies retained by downstream nodes preserve their previous values. Shared
immutable storage remains alive until its last owner releases it.

`Out<TS<T>>::apply(const literal::ScalarLiteral&)` constructs a `ScalarValue`
through `from_literal()` and passes it to the consuming `set_value()` overload.
A scalar leaf replaces the value. A brace sequence constructs
a complete replacement using the [scalar literal syntax](scalar-api.md#literals);
nested values use the actual destination runtime types. An empty collection
sequence publishes a valid empty collection. Equal replacements are suppressed
only while a deduplicated output is valid, so applying an equal value
after invalidation revalidates it. Concrete and generic TS outputs preserve the
selected runtime representation and their containing output path.

```cpp
void replace_list(Out<TS<List<i64>>> output) {
    output.apply({1, 2, 3});
}
```

`Out<TS_T>::apply()` dispatches to its resolved scalar time series output.
Other time series kinds throw `std::invalid_argument`.
Follow the [literal lifetime and construction contract](scalar-api.md#literals).

`Out<>::copy_tick(input)` applies the input's current-cycle changes to the output.
It does nothing when the input has no change in the current evaluation cycle.
Collection copies apply additions, removals, and changed items recursively.
They do not replace unchanged items or reconstruct a missed earlier tick.

`Out<>::copy(input)` copies the input's complete current value. It removes
collection entries absent from the input and copies every current item.
Use it when sampling a value rather than forwarding a tick.

The input must have a compatible value shape. Its time series guarantees need
not match the output's guarantees. Both operations use normal output mutation
and guarantee behavior. Invalid inputs invalidate outputs that support
invalidation. For an invalid TSS or TSD input, `copy_tick()` applies reported
removals and `copy()` clears an existing output. Neither operation initializes
an invalid output from an invalid input. A cleared output remains valid,
following the [validity design](time-series-validity.md).
Reference copies copy the reference, not its target value.
`Out<REF<TS_T>>::set_value(reference)` assigns a `Reference` directly.

Typed TSD copying preserves `valid_items`. Runtime-dispatched copying uses the
underlying input binding without that filter. This includes TSD fields copied
as part of a whole TSB copy.

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
