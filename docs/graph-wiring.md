# Graph wiring

## Purpose

Graph wiring records node calls and their connections before graph compilation.
`GraphWiring` owns and indexes wiring records, wired outputs, and wired nodes.
Wiring records materialize values. Node templates construct wired nodes.

## Wiring points

A `WiringPoint` identifies a wiring record associated with one `GraphWiring`.
The `GraphWiring` owns records that can contribute to the graph. Signature
resolution can use records owned by a disposable overlay associated with the
same `GraphWiring`. These records provide ordinary wiring points for type
resolution and requirements, but cannot materialize or escape the operation
that owns the overlay.

A node call with an output returns a record for that output. Free collections
use records that retain their item wiring points until a node consumes the
collection. A call without an output returns `void`.

A wiring record owns behavior required to materialize its value. A record may
return a compatible lazy point; `GraphWiring::materialize` follows it until it
reaches a wired output or a direct input binding. It checks graph ownership,
type compatibility, and the final binding before returning. Graph compilation
consumes the resulting input bindings and does not depend on wiring-record
behavior. A node template can identify a wired output for
compiler lowering without changing its wiring representation.
A materialized record whose layout differs from the normal layout for its
semantic type stores its runtime type on the wired output. Graph compilation
uses a retained output runtime type or resolves the default from the output's
semantic type.

Each `WiringPoint` argument to a node call must belong to the active
`GraphWiring`. Inner graph boundaries provide local wiring points for their
declared inputs.

`wp<TS_T>` adds a time series type to a wiring point. It has the same runtime
size as `WiringPoint` and converts to `WiringPoint`. Graph definitions use it to
declare time series parameter and output types.

`wp<TS_T, optional>` declares an optional graph input. Forwarding this input
requires an optional parameter on the receiving node, graph, or operator
overload. `opt(point)` supplies this annotation at a call site. It keeps the
time series type and identity and adds no wiring-point storage. See
[operator optional arguments](operators.md#optional-arguments) for selection
and invalid-input behavior.

`wp<TSD<K, TS_T>, valid_items>` declares a graph input that requires the
receiving input view to apply the
[`valid_items` guarantee](time-series-validity.md#collections).
`valid_items(point)` supplies this annotation at a call site. Like `opt`, it
retains the time series type and identity and adds no node or wiring-point
storage. Both annotations survive graph forwarding, context arguments, and
materialisation.

`point.as<TS_T>()` returns a typed wiring point when the requested type accepts
the point's declared type or an advertised materialisation target. Constructing
`wp<TS_T>` from another wiring point performs the same check using
[input guarantee rules](type-system.md#time-series-guarantees). Both operations
retain the original point and actual type without selecting a materialisation.
`as<TS_T>()` preserves the point's `optional` and `valid_items` annotations.
Typed wiring-point conversions reject discarding either annotation.

`point[key]` forwards to `operators::getitem` and returns its `wp<>` unchanged.
The selected overload determines the result's semantic type, including its
reference and target guarantees. A scalar index into a
fixed TSL selects its contained wiring point without adding a node. A time
series index, or a TSD key supplied as either a scalar or time series, creates a
node that tracks the selected item.

`bg::tsl(a, b, ...)` groups item wiring points without creating item storage.
The receiving parameter determines whether each item is a value or a reference.
Wiring then creates one TSL output over the selected items.
Inferred value items use the [dereferenced guarantees](type-system.md#reference-guarantees)
that consumers can observe, rather than a reference target's guarantees alone.

`bg::tsl(items)` accepts a vector or span of `WiringPoint` or typed `wp`
items. Its fixed size is the number of supplied items. An empty list requires
an explicit item type: `bg::tsl<TS<i64>>()` or `bg::tsl(item_type)`, where
`item_type` is a resolved `TimeSeriesType`.

The logical output preserves dependency ordering. Item updates schedule it as a
normal node. It combines updates from the same cycle and copies no item value.

`bg::bundle("name"_n = a, "value"_n = b)` and `bg::bundle(fields)` create the
same free TSB wiring point. Both forms accept named time series and scalar
fields. Wiring creates one TSB output when the bundle is consumed; scalar
fields project to const time series. `args(bundle)` and `kwargs(bundle)` retain
the original scalar arguments and wiring annotations when expanding the
bundle. Field names can be determined during wiring:

```cpp
auto fields = std::vector<bg::WiringArgument>{};
fields.emplace_back(bg::util::symbol{"price"}, price);
fields.emplace_back(bg::util::symbol{"quantity"}, 10);
auto result = bg::bundle(fields);
```

The bundle retains its time-series wiring points and owns scalar values; the
vector or span's underlying storage need not outlive the call. An absent
optional scalar field is omitted. An empty field range constructs an empty
TSB.

`bg::bundle<Schema>(arguments...)` uses the declared schema. When every
argument is a scalar and `Schema` is a valid hierarchical scalar schema, it
constructs `ScalarValue<Bundle<Schema>>`. A wiring point or type-erased wiring
argument selects `wp<TSB<Schema>>`; scalar fields are lifted to const time
series. If only the TSB interpretation is valid, scalar arguments also select
it.

`bg::bundle<TSB<Schema>>(arguments...)` explicitly constructs a TSB and lifts
all scalar fields. This form is required to construct an all-constant TSB from
a schema that is also valid for `Bundle<Schema>`.

`wp<TSB<Schema>>` exposes the schema fields through `operator->`. Each field is
a typed wiring point. A field becomes a wired output only when a node consumes
it. `operators::getattr` selects a field by name from named and unnamed TSBs and
returns `wp<>`. Re-annotate an unnamed TSB with `as<TSB<Schema>>()` to use the
schema's C++ field members.

## Deferred wiring points

Extensions can create a deferred wiring point with a resolved semantic type
and retained original arguments. The point can advertise one materialisation
option per exact target type. Each option has a finite, nonnegative cost, an
input signature, and a materialisation callback. Typed callbacks derive their
input signature from their parameters. Erased callbacks require an explicit
`Signature`. Declare every upstream input required by the callback in its
parameters or erased input signature.
Inspecting, preparing, or expanding an option fixes the option set. The borrowed
input-signature pointer in an inspected option remains valid while its owning
`GraphWiring` lives.

Preparation normalizes the retained arguments once and completes the option's
input signature before a materialisation callback runs. Concrete input
parameters receive the selected direct or materialized form. Union parameters
receive the original point with its materialisation options intact. Callback
binding cost ranks only selected concrete inputs; Union input forms remain
open. Signature matching continues to report its full cost across all matched
forms.

A deferred point does not provide its semantic type as a direct representation.
Consumers can use only its advertised options. Ordinary wiring points provide
their semantic type directly, including lazy bundles.

`GraphWiring::expand_materialisation` runs the selected materialisation
callback immediately and caches its result by source point and target type.
The result may remain a lazy wiring record. A callback that wires graph
operations requires the caller to activate the owning graph's `WiringContext`.
The deferred point retains its creation path and lexical context and restores
them when its callback runs.

Call `GraphWiring::require_materialisation` to register each root demand before
expansion. It normalizes and prepares the root and recursively selected
concrete callback inputs once, without running materialisation callbacks. Union
inputs do not add a concrete demand. `materialisation_required` checks the
recorded source identity and target type. Expansion also works without a
registered demand; that local preparation does not mark the point as required.
Collection retains prepared callback arguments and input bindings so expansion
uses the same graph-local sources. Preparation can import an implicit context
provider from a parent graph.

Deferred points preserve their semantic type; options do not add implicit
materialisations. Retained arguments and expansion results must belong to the
same `GraphWiring` and cannot use records owned by a temporary resolution
overlay. Generic semantic types must be resolved before creating a deferred
point.

`Signature::match_wiring_arguments` accepts normalized arguments, with one
entry per signature parameter; a variadic capture remains one argument. It
considers each available direct form and each advertised materialisation target
without running materialisation callbacks or completion callbacks. Each match records its
offered forms, type map, and total materialisation cost. The total counts an
identical source point and target type once.

Before enumerating offered forms, matching probes each original argument type
against its parameter in an independent type map. These probes ignore time-series
guarantees and do not use the explicit seed. A variable other than a guarantee
variable is constrained by the original arguments only when at least one
successful probe maps it and every successful probe that maps it produces the
same mapping after guarantees are removed recursively. Conflicting
source mappings leave that variable unconstrained. A candidate is retained when
normal matching with the explicit seed succeeds and its mappings satisfy all
source constraints. This preserves compatible source structure without rejecting
a materialisation when the original type cannot match the parameter.

`Signature::complete_wiring_match` consumes one match and completes its
resolvers and supplied completion callback in the same scoped resolver
environment as ordinary signature completion. Its argument view retains the
original descriptors and scalar-read tracking while `resolved_type()` reads the
match's type map. It returns the resolved signature and map together with the
selected forms and cost. Operator and graph calls use this matching without
running materialisation callbacks. Original argument structure constrains
compatible mappings, but materialisation forms still determine the concrete
types offered to the signature.

## Node construction

A node call resolves its signature and validates its arguments before retaining
the call and its concrete input requirements. Missing required scalars, graph
arguments passed to nodes, and incompatible output runtime selections fail at
the call. Output runtime selection runs once; concrete binding reuses its
result.

`GraphWiring::finalize()` completes materialisations, node binding, and service
construction before compilation. Calls introduced during completion receive
the same validation when introduced. A committed materialisation remains fixed;
additional consumers can request another advertised form. Completion includes
outputless calls and calls whose outputs are unused.

Successful `finalize()` closes wiring. Repeating it is harmless;
construction after it is an error. A failed completion stops construction.
`complete_pending_wiring()` is an engine operation that completes current work
while permitting later wiring. Compilation requires a successful `finalize()`
and does not construct nodes. `run_graph` and `test_graph` complete wiring
automatically.

## Wiring context

`WiringContext` makes one `GraphWiring` active on the current thread:

```cpp
bg::GraphWiring wiring;

{
    bg::WiringContext context{wiring};

    auto input = source();
    auto output = transform(input);
}
```

Contexts can nest. Leaving a context restores the previous active
`GraphWiring`. A wiring-template call requires an active context.

The current context exposes its `GraphWiring` and wiring path. A
wiring-template call activates a child context while its implementation runs.

## Node call

A node call:

1. checks that each wiring point belongs to the active `GraphWiring`
2. resolves the signature from the argument types
3. records each time series as a local wired output or inherited context
   requirement, materializing local wiring points for the resolved type
4. reuses an existing wired node with the same node template, resolved types,
   wired inputs, state types, and scalar arguments when permitted
5. otherwise stores those values in a new wired node
6. returns a `WiringPoint` when the wired node has an output

Scalar arguments are immutable wired-node data and do not create input wired
inputs. `GraphWiring` owns a copy of each supplied scalar. An optional scalar
parameter can record absence.

Node calls with no output are never reused. A scalar without equality and
hashing also prevents reuse. The first call supplies the retained wiring path.
Set `deduplicate_wiring` to `false` on a node definition when equal calls must
create distinct wired nodes.

## Context time series

A context parameter requests a named time series without adding it to each
intermediate graph call. Callable definitions put the name and `context` on the
parameter:

```cpp
using namespace bg::literals;

auto use_one = bg::node([](bg::In<bg::TS<int>, "one"_n, bg::context> one,
                           bg::Out<bg::TS<int>> output) {
    output.set(*one);
});
```

An aggregate node uses its reflected field name:

```cpp
struct UseOne {
    bg::In<bg::TS<int>, bg::context> one;
    bg::Out<bg::TS<int>> output;
};
```

`with_context` establishes a lexical binding for graph code. A scalar value is
lifted to a const time series.

```cpp
bg::with_context("one"_n = value, [&] { return graph_x(); });
auto scope = bg::with_context("one"_n = 1);
```

The nearest binding with the requested name is authoritative. An incompatible
nearest binding is an error. Wiring also fails when no binding has that name. A
named argument supplied directly to a context parameter overrides the lexical
binding. Context parameters do not accept positional arguments and cannot be
variadic or dynamically bound.

A graph context parameter also establishes that binding while its graph
function runs:

```cpp
auto graph_x = bg::graph(
    "name"_n = "graph_x",
    [](bg::wp<bg::TS<int>, "one"_n, bg::context>) { return use_one(); });

graph_x("one"_n = value);
```

Type resolution treats the selected context value as an ordinary argument.
Operator resolution cache keys include the presence and type of every implicit
context value that can affect overload selection.

Engine code can capture visible lexical bindings with
`details::ContextSnapshot`. A snapshot owns copies of scalar arguments and
borrows graph-owned time-series wiring points. It does not extend the owning
`GraphWiring` lifetime; that graph must outlive snapshot use. Activation exposes
only the saved bindings, including when the snapshot is empty, and restores the
prior context when the activation ends.

## Wiring template call

`WiringTemplate` is the common base of node templates and graph templates. A
call:

1. resolves and checks its signature
2. calculates its label
3. creates a child wiring path and activates its wiring context
4. invokes the template implementation
5. checks the returned output against the resolved signature

A node template adds one wired node. A graph template invokes its graph function
and can add any number of wired nodes.

Engine and extension implementations override:

```cpp
const Signature &default_signature() const;

std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature,
                               const WiringArguments &argument_view,
                               std::span<const WiringArgument> arguments) const;
```

`signature()` returns the template's current signature. `set_name(symbol)`
creates a renamed copy for this template without changing its definition's
shared metadata or other template copies. Set the name before wiring calls.
Custom templates provide their original signature through `default_signature()`.

`argument_view` provides resolved mappings and the fixed and flattened variadic
arguments used by requirements and resolvers. `arguments` contains one normalized
argument per signature parameter for forwarding and node storage. Both are
borrowed for the duration of the callback. Use `argument_view.type_map()` to
access the resolved mappings.

An [operator](operators.md) selects a node template or graph template before
invoking its ordinary wiring behavior. Overload resolution remains internal to
the operator.

Callable nodes and graphs can declare ordered type parameters before their
function. Aggregate forms declare them before the definition:

```cpp
using RESULT = bg::TimeSeriesVariable<"RESULT">;

auto callable_node = bg::node<RESULT>(function);
auto callable_graph = bg::graph<RESULT>("name"_n = "source", function);

auto aggregate_node = bg::node<RESULT, NodeDefinition>();
auto aggregate_graph = bg::graph<RESULT, GraphDefinition>();
```

A callable graph may take `const WiringArguments&` as its first function
parameter. Wiring supplies the resolved invocation metadata; this parameter is
not part of the graph signature. A graph can pass its resolved type map to
another type-parameterized wiring construct with `implementation[arguments.type_map()]`.
`nested()` accepts that mapped graph and resolves its output-only type
parameters from the supplied map.

Declarations without ordered type parameters keep their existing form. The
[type system](type-system.md#signature-resolution) defines explicit type
arguments and named mappings on calls.

`GraphWiring` retains a reference to each supplied node template. A
node-template call therefore requires an lvalue whose lifetime includes graph
compilation. A runtime-signature node template can be called as a temporary;
the active `GraphWiring` owns it.

## Graph templates

A graph function constructs graph code by invoking wiring templates. A graph
template gives that function a checked signature and a diagnostic boundary.
Its call expands into the active `GraphWiring`.

Graph structure comparison treats declared inputs as ordered external
boundaries. Their producer stub nodes are not part of the structure. All other
nodes remain part of it, including outputless nodes. Boundary types, the result,
node definitions, connections, and stored scalar values are semantic. Local
node indices, wiring paths, and parameter names are not.

`graph(...)` infers the signature from the graph function. A time series
parameter uses `wp<TS_T>`. Scalar parameters use their scalar types. The return
type is `wp<TS_T>` or `void`.

Callable and aggregate graph input parameters may use
`wp<Union<"FORM", TS_A, TS_B>>` when every alternative is a supported time
series declaration. Signature resolution checks the supplied time series
against the alternatives, and the graph function receives that same wiring
point with the union annotation. Graph outputs continue to use `wp<TS_T>` or
`void`.

A higher-order graph uses `GRAPH<auto(parameters...) -> result>` for a graph
parameter. The declaration checks the supplied graph boundary signature and
passes its variables through normal type resolution. Structural constraints
use [`graph_pattern`](operators.md#graph-arguments).

`GraphArgument` owns its graph structures and operation-specific payload.
For one structure, `exact_type()` returns a process-lifetime boundary signature
type. Different structures can have the same type. `structure()` requires one
graph structure. Operations can also supply an ordered collection of structures,
which uses the any-graph type.

For engine and extension implementations, `Parameter::graph_constraint` owns
an optional `GraphArgumentConstraint` containing exact structures or a wiring
template pattern. The signature retains it through shared ownership. `GRAPH`
declarations attach their constraints automatically. Matching checks both the
parameter type and its constraint before invoking the graph function.

A lambda graph infers its name from the callable type unless `"name"_n`
supplies a name. Its optional label uses `"label"_n`; its optional requirement
uses `bg::require`. The graph function is the only positional argument:

```cpp
using namespace bg::literals;

inline const auto scaled = bg::graph(
    "name"_n = "scaled",
    "label"_n = [](bg::wp<bg::TS<float>>, float factor) {
        return std::to_string(factor);
    },
    bg::require = [](const bg::WiringArguments &arguments) {
        return bg::scalar_cast<float>(*arguments.scalar(1)).value() > 0.0F;
    },
    [](bg::wp<bg::TS<float>> input,
       float factor) -> bg::wp<bg::TS<float>> {
        return scale(input, factor);
    });
```

The label function receives the graph function's arguments. Lambda signature
parameters use their zero-based positions as names. The requirement receives a
`WiringArguments` view after signature resolution and before graph-body wiring.
See [operator requirements](operators.md#requirements) for the shared callback
protocol and caching rules.

An aggregate struct provides the graph name and parameter names through
reflection. Its fields declare the parameters. Its `wire()` function constructs
the graph code and returns `wp<TS_T>` or `void`:

```cpp
struct scaled {
    bg::wp<bg::TS<float>> input;
    float factor;

    bg::wp<bg::TS<float>> wire() const {
        return scale(input, factor);
    }
};

inline const auto scaled_graph = bg::graph<scaled>();
```

## Direct execution

`run_graph(function)` wires and compiles the graph, then runs it with the selected
evaluation engine. Pass `GraphRunOptions` from
`<bg/graph_run_options.hpp>` as the second argument to configure the run:

```cpp
auto options = GraphRunOptions{
    .global_state = &state,
    .services = &catalogue,
    .recorder = profiler.recorder(),
};
run_graph(function, options);
```

All fields are optional. `global_state` selects the [global state](global-state.md),
`services` selects the [service catalogue](services.md), and `recorder` enables
[profiling](evaluation.md#evaluation-profiling). The options borrow these objects;
keep them alive throughout the synchronous run.

`engine` selects `EvaluationEngine::simulation` by default. Simulation returns
when no scheduled evaluation remains. Select `EvaluationEngine::realtime` to
wait for queue input and timer deadlines until `stop_token` requests cancellation:

```cpp
auto stop_source = std::stop_source{};
run_graph(function, GraphRunOptions{
    .engine = EvaluationEngine::realtime,
    .stop_token = stop_source.get_token(),
});
```

The application requests stop through the source. Realtime cancellation follows
the [engine lifecycle](evaluation.md#realtime-evaluation).

`start_time` overrides the first evaluation time. When omitted, simulation uses
`MIN_DT + 1us`; realtime samples the current wall-clock time after graph
compilation. Profiling uses the selected engine and start time.

`periodic_callback` configures an optional [periodic callback](evaluation.md#periodic-callbacks)
for either engine.

[`testing::test_graph`](testing.md#test-layout) accepts the same options
immediately after the graph and requires simulation execution.

## Wiring paths

Each wiring-template call creates an immutable path entry:

```cpp
struct WiringPath {
    const WiringPath* parent;
    util::symbol name;
    util::symbol label;
};
```

The signature supplies the name. The default label is a number generated by the
owning `GraphWiring`. A template can supply a label function that calculates a
symbol from its wiring-time arguments. `node<Definition>(label_function)` adds
one to an aggregate node template. Labels are diagnostic descriptions and can
repeat.

`GraphWiring` owns path entries in stable storage. A wired node refers to the
entry for its node-template call.

## Wiring errors

Validate calls as soon as the required declared information is available.
Materialisation callbacks run when their result is required. Failures that need
callback execution include invalid returned points, invalid generated calls,
and dependencies first introduced by expansion.

`WiringError::contexts()` describes the operations leading to a failure. Context
includes the graph path, operation, consumer parameter or collection item,
requested type, and expected and actual result types where applicable. Context
paths and type descriptions are owned by the error. `what()` renders the failure
and its contexts.
`diagnostic()` retains the structured [operator diagnostic](operators.md#diagnostics).
`cause()` retains an underlying callback exception. `with_context()` adds context
while preserving the existing diagnostic and cause.

The engine/extension overload of `GraphWiring::require_materialisation` accepts
a `WiringRequestOrigin` identifying the consumer and parameter. Node and selected
overload input binding supply this origin automatically. Shared construction
retains distinct requesting origins. Deferred node calls and service requests
retain causal context so their failures identify the requests that introduced
them. Builder signatures do not need diagnostic parameters.
