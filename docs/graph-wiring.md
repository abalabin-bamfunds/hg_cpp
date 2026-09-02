# Graph wiring

## Purpose

Graph wiring records node calls and their connections before graph compilation.
`GraphWiring` owns the resulting wired nodes.

## Wiring points

A node call appends one wired node to the active `GraphWiring`. A call with an
output returns a `WiringPoint` containing its owning `GraphWiring` and
wired-node index. A call without an output returns `void` in the typed C++ API
and `None` in Python.

Each `WiringPoint` argument to a node call must belong to the active
`GraphWiring`. Inner graph boundaries provide local wiring points for their
declared inputs.

`wp<TS_T>` adds a time series type to a wiring point. It has the same runtime
size as `WiringPoint` and converts to `WiringPoint`. Graph definitions use it to
declare time series parameter and output types.

`bg::tsl(a, b, ...)` groups wiring points without creating time series storage.
The receiving parameter determines whether each item is a value or a reference.
Wiring then creates one logical TSL output that aliases the selected sources.
Selecting an item from that output returns the original source wiring point.

The logical output preserves dependency ordering. Source changes schedule it as
a normal node. It combines changes from the same cycle and copies no item value.

`bg::bundle("name"_m = a, "value"_m = b)` groups wiring points into an
unnamed TSB. A named TSB parameter accepts a compatible field set. The logical
TSB output aliases the selected sources and reports their field changes.

Graph code selects a named field with `bundle["name"_m]`. Selection returns the
field's original wiring point when the TSB was created by `bg::bundle`.

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
3. stores the node template, resolved signature, and input source indices in a
   wired node
4. returns a `WiringPoint` when the wired node has an output

Scalar arguments are immutable wired-node data and do not create input source
indices. `GraphWiring` owns a copy of each supplied scalar. An optional scalar
parameter can record absence.

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

`GraphWiring` retains a reference to each supplied node template. A
node-template call therefore requires an lvalue whose lifetime includes graph
compilation. A runtime-signature node template can be called as a temporary;
the active `GraphWiring` owns it.

## Graph templates

A graph function constructs graph code by invoking wiring templates. A graph
template gives that function a checked signature and a diagnostic boundary.
Its call expands into the active `GraphWiring`.

`graph(...)` infers the signature from the graph function. A time series
parameter uses `wp<TS_T>`. Scalar parameters use their scalar types. The return
type is `wp<TS_T>` or `void`.

A lambda graph requires a `"name"_m` argument. Its optional label uses
`"label"_m`. The graph function is the only positional argument:

```cpp
using namespace bg::literals;

inline const auto scaled = bg::graph(
    "name"_m = "scaled",
    "label"_m = [](bg::wp<bg::TS<float>>, float factor) {
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
until no future evaluation remains. `run_graph(function, state)` uses the
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

## Open implementation choices

1. Recovery when a wiring-template call fails after adding wired nodes.
