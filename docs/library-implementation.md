# Library implementation guidelines

## Purpose

Use these guidelines when implementing library operator overloads. Prioritize
correct graph evaluation, execution performance and memory use, then readable
code, following the [project priorities](project.md#priorities).

Examples omit includes and use `using namespace bg;`. Class excerpts belong
inside an aggregate node definition.

## Define observable behavior

Before selecting framework features, specify:

- which changes schedule evaluation and which validity conditions permit it
- behavior before the first valid input, on invalidation, and on revalidation
- when the output publishes, preserves its value, or becomes invalid
- handling of collection additions, removals, and item changes
- the output guarantee and error behavior

For example, sampling publishes the complete current value whenever its trigger
ticks. Changes to the sampled input alone do not schedule it:

```cpp
const auto sample = node(
    [](In<TS<bool>>, In<TSD<i64, TS<double>>, passive> value,
       Out<TSD<i64, TS<double>>> output) {
        output.copy(value);
    });
```

The first input is the trigger. Both inputs require validity. An ordinary tick,
a suppressed deduplicated update, and no update have different observable
behavior even when their values are equal.

## Choose the definition form

Use a graph overload when the operation composes existing operators or returns
an existing wiring point:

```cpp
const auto identity = graph(
    [](wp<TS<double>> value) {
        return value;
    });
```

Use a callable node for a small stateless calculation. Plain parameters suit
cheap values that can arrive as either scalars or time series:

```cpp
const auto add = node(
    [](double lhs, double rhs) {
        return lhs + rhs;
    });
```

Use `In<>` for borrowed access, validity, change information, or input
properties. Use `Scalar<>` for a parameter supplied only at wiring time:

```cpp
const auto scale = node(
    [](In<TS<double>> value, Scalar<double> factor) {
        return *value * *factor;
    });
```

Use an aggregate for persistent state or lifecycle functions. Use a
runtime-signature definition when its parameter structure becomes known only
at runtime. The [node API](node-api.md) defines each form and the lifecycle
restrictions on plain promoted parameters.

Use a dedicated node when graph composition has a demonstrated execution or
memory cost that the node can reduce.

## Select input validity

Use `required` when the calculation needs a valid root value. This is the
default, and the framework gates evaluation:

```cpp
In<TS<double>> value;
```

Use `all_valid` when the calculation reads every immediate collection item:

```cpp
const auto sum_pair = node(
    [](In<TSL<TS<double>, 2>, all_valid> values) {
        return *values[0] + *values[1];
    });
```

The framework checks immediate items before each scheduled evaluation. For
nested collections, specify how the algorithm handles deeper validity. See
[validity constraints](time-series-validity.md#nodes).

Use `optional` when invalidity has a defined meaning for the operation:

```cpp
const auto add_optional = node(
    [](In<TS<double>, optional> lhs,
       In<TS<double>, optional> rhs) {
        const auto left = lhs.valid() ? *lhs : 0.0;
        const auto right = rhs.valid() ? *rhs : 0.0;

        return left + right;
    });
```

This node evaluates on input changes. Optional validity permits evaluation
with invalid inputs; startup scheduling is a separate choice.

Register optional implementations as overloads so `opt()` can select them.
Define missing-value behavior for each operation. Multiplication, selection,
and addition can have different behavior.

Use `default_()` when graph code requires a specific fallback value independent
of the consuming operation. See [optional arguments](operators.md#optional-arguments).

## Select input activity and scheduling

Use active inputs when their changes should recompute the result. Use passive
inputs when another event determines when to sample them, as in `sample` above.

Use `dynamic_activity` when the algorithm changes its interest in an
already-wired input. An operation that accepts only its first valid value can
stop scheduling from later updates:

```cpp
struct first_value {
    static constexpr auto on_invalidation = preserve_output;

    In<TS<double>, dynamic_activity> value;
    Out<TS<double, TimeSeriesGuarantee::final>> output;

    void evaluate() const {
        output.copy(value);
        value.make_passive();
    }
};
```

Activity controls scheduling. Validity checks and input callbacks retain their
normal behavior. This node preserves its final output on later input
invalidation.

Use `schedule_on_start` when the operation requires an initial evaluation
without an input tick. For example, this source publishes the initial
evaluation time:

```cpp
struct startup_time {
    static constexpr bool schedule_on_start = true;

    Out<TS<date_time_t>> output;

    void evaluate(EvaluationContext *context) const {
        output.set(context->evaluation_time());
    }
};
```

The framework schedules that evaluation in the first cycle.

Use timed scheduling when evaluation must happen without another input tick.
Declare its use on the aggregate:

```cpp
static constexpr bool uses_timed_scheduling = true;
```

Schedule through the invocation's `EvaluationContext`:

```cpp
context->schedule_at(
    context->evaluation_time() + std::chrono::seconds{1});
```

Use evaluation time for graph timing and the context's wall clock for
wall-clock observations. The [evaluation contract](evaluation.md) defines
scheduling, lifecycle, cleanup, and failure behavior.

## Propagate output guarantees

Use a guarantee variable when the complete behavior supports retaining the
input guarantee:

```cpp
inline constexpr auto G = guarantee_variable<"G">;

const auto negate = node(
    [](In<TS<double, G>> value, Out<TS<double, G>> output) {
        output.set(-*value);
    });
```

This input-driven transformation has no independent scheduling. A deduplicated
output uses the framework's equality-based suppression.

Use the same `G` across inputs when weakest-guarantee resolution expresses the
intended relationship. Use a resolver when the output needs a different rule.

Include optional inputs, timers, startup, and invalidation in that reasoning.
Two required `final` inputs can support one final result. Two optional `final`
inputs can arrive separately and produce multiple results.

For references, reason separately about reference changes and target updates.
For collections, include the guarantee's effect on contained items. Use the
[guarantee rules](type-system.md#time-series-guarantees) and normal output
enforcement.

## Select output operations

Use a returned scalar when each evaluation produces one value. Use
`std::optional<T>` when an evaluation can deliberately preserve the existing
output:

```cpp
const auto positive = node(
    [](double value) -> std::optional<double> {
        if (value <= 0.0) {
            return std::nullopt;
        }

        return value;
    });
```

Use `Out<>` for collection mutations, reference routing, explicit guarantees,
or explicit invalidation. Select copying according to the observation required:

```cpp
output.copy_tick(input); // Forward this cycle's changes.
output.copy(input);      // Sample the complete current value.
```

Use change queries for incremental algorithms that maintain a matching
baseline. Use complete enumeration when the algorithm needs the complete value.
When processing a validity transition manually, establish that it belongs to
the current cycle; transition queries can describe an older event.

Select `preserve_output` when a required input's invalidation should leave the
output unchanged. Use explicit invalidation when the contract removes the
current value. Clearing a TSS or TSD produces a valid empty collection.
See [output operations](node-api.md#output-parameters).

## Use references and dynamic binding for runtime selection

Use a reference output when the result identifies an existing time series.
Downstream consumers can then observe its updates without copying its values.

For a reference supplied through graph wiring, ordinary `In<>` bindings handle
retargeting.

Use `dynamic_binding` when the node selects an addressable input during
evaluation and needs subscription management for that selection. This is rare
in library implementations. Engine `getitem` implementations are an example:

```cpp
struct select_item {
    In<REF<TSD<i64, TS<double>>>> values;
    In<TS<i64>> key;
    In<REF<TS<double>>, dynamic_binding, optional> item;
    Out<REF<TS<double>>> output;

    void evaluate() const {
        item.bind(values[key.value_view()]);
        output.copy(item);
    }
};
```

The internal `item` is excluded from the public signature. Binding establishes
the required subscriptions; the framework manages replacement and cleanup.

For ordinary library composition, use the existing `getitem` operator in graph
code. Use this implementation pattern for operations that own runtime
selection. See [dynamic inputs](node-api.md#complete-time-series-access) and
[reference selection](time-series-references.md#collection-item-selection).

## Retain information with the required lifetime

Use `State<>` for information that must survive invocations:

```cpp
struct running_sum {
    In<TS<double>> value;
    State<double> total;
    Out<TS<double>> output;

    void evaluate() const {
        *total += *value;
        output.set(*total);
    }
};
```

This operation accumulates valid evaluations and retains its total across input
invalidation. Specify reset behavior when the operation needs it.

Use owning scalar values with the required allocator lifetime when retaining
values. Use [tick capture and replay](time-series-types.md#retaining-ticks) when
retaining changes for later publication. Input, output, and borrowed scalar
views remain invocation-scoped. The [scalar ownership contract](scalar-api.md#ownership)
defines allocation and lifetime requirements.

Use scoped cleanup for acquisitions and lifecycle functions for resources
whose lifetime spans invocations. Make cleanup valid during failure and stop.
Retain data already available from inputs or output only when the algorithm
requires an independent value or measured performance justifies a cache.

## Match implementation to type information

Use `In<>` and `Out<>` for time series access in node code, and `Scalar<>` for
scalar access. Use typed view operations for known types and visitors for
generic structural algorithms. These APIs handle storage representations;
runtime types and providers implement the dispatch behind them.

Express type relationships and capabilities in signatures. Use requirements
for wiring-time value constraints and resolvers for types that argument
matching cannot determine. Their results must depend on supplied wiring
arguments and the type map. Obtain types through declarations or registries.
See [resolvers and requirements](operators.md#resolvers).

Export native operators through the existing
[Python operator interface](python.md#c-extensions) so both languages use the
same implementations.

Raise an unavailable framework operation or abstraction gap with the project
owner before choosing a library-specific workaround.

## Verify behavior, cost, and readability

Start with a complete graph test:

```cpp
CHECK(testing::test_graph(
    add, {1.0, 2.0}, {10.0, 20.0}).equals({11.0, 22.0}));
```

Add cases for the transitions and representations relevant to the contract:
initial invalidity, simultaneous changes, equal values, revalidation, removals,
guarantees, references, and nested boundaries. Test optional behavior separately
from fallback behavior. Follow the [testing guidelines](testing.md).

Assess performance through nodes introduced, work per changed item,
allocations, retained state, copying, and subscriptions. Measure alternatives
when those costs differ materially.

Keep implementations in execution order and separate logical parts with blank
lines. Spell composite time series types directly and name type variables in
uppercase. Give each helper a responsibility that makes its caller smaller and
clearer.
