# Graph wiring

## Purpose

Graph wiring records node calls and their connections before graph compilation.
`GraphWiring` owns and indexes wiring records, wired outputs, and wired nodes.
Wiring records materialize values. Node templates construct wired nodes.

## Wiring points

A `WiringPoint` identifies a wiring record in its owning `GraphWiring`. A node
call with an output returns a record for that output. Free collections use
records that retain their item wiring points until a node consumes the
collection. A call without an output returns `void`.

A wiring record owns behavior required to materialize its value. Materialized
records identify wired outputs. Graph compilation consumes only wired outputs
and does not depend on wiring-record behavior. A materialized record whose
layout differs from the normal layout for its semantic type stores its runtime
type on the wired output. Graph compilation resolves the runtime type of other
outputs from their resolved semantic types and input runtime types.

Each `WiringPoint` argument to a node call must belong to the active
`GraphWiring`. Inner graph boundaries provide local wiring points for their
declared inputs.

`wp<TS_T>` adds a time series type to a wiring point. It has the same runtime
size as `WiringPoint` and converts to `WiringPoint`. Graph definitions use it to
declare time series parameter and output types.

`point.as<TS_T>()` returns a typed wiring point when the requested type accepts
the point's runtime type. Constructing `wp<TS_T>` from another wiring point
performs the same check.

`point[key]` forwards to `operators::getitem`. Operator overload resolution
selects the behavior for the collection and key types. A scalar index into a
fixed TSL selects its contained wiring point without adding a node. A time
series index, or a TSD key supplied as either a scalar or time series, creates a
node that tracks the selected item.

`bg::tsl(a, b, ...)` groups item wiring points without creating item storage.
The receiving parameter determines whether each item is a value or a reference.
Wiring then creates one TSL output over the selected items.

The logical output preserves dependency ordering. Item updates schedule it as a
normal node. It combines updates from the same cycle and copies no item value.

`bg::bundle("name"_n = a, "value"_n = b)` creates a wiring point for an
unnamed TSB. A named TSB parameter accepts a compatible field set. Wiring
creates one TSB output over the selected fields and reports their field
changes.

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

A higher-order graph uses `GRAPH<auto(parameters...) -> result>` for a graph
parameter. The declaration checks the supplied graph boundary signature and
passes its variables through normal type resolution. Structural constraints
use [`graph_pattern`](operators.md#graph-arguments).

A lambda graph requires a `"name"_n` argument. Its optional label uses
`"label"_n`. The graph function is the only positional argument:

```cpp
using namespace bg::literals;

inline const auto scaled = bg::graph(
    "name"_n = "scaled",
    "label"_n = [](bg::wp<bg::TS<float>>, float factor) {
        return std::to_string(factor);
    },
    [](bg::wp<bg::TS<float>> input,
       float factor) -> bg::wp<bg::TS<float>> {
        return scale(input, factor);
    });
```

The label function receives the graph function's arguments. Lambda signature
parameters use their zero-based positions as names.

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

`run_graph(function)` wires and compiles the graph, then runs it as a simulation
until no scheduled evaluation remains. `run_graph(function, state)` uses the
supplied [global state](global-state.md), which can outlive the run.

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
