# Map

## Purpose

`map(graph, arguments...)` runs one inner runtime graph for each selected key.
It compiles the graph once and creates a separate runtime instance for every
current key.

The first argument can be a graph template or a graph function. A graph
template accepts typed or untyped wiring points because wiring resolves their
semantic types. A C++ graph function requires typed TSD and TSS arguments so
that C++ can derive its per-key parameter types before invoking it.

Map's operator implementation converts a graph function to a graph argument
before normal overload resolution. This keeps generic lambdas unannotated while
allowing overloads to match the resulting graph signature and structure.

## Runtime calls

A caller that obtains the graph template and arguments at runtime passes a
`WiringTemplate` and a span of `MapArgument` values. `WiringArgument` retains
the semantic type and value without exposing its original C++ wrapper type.
`MapArgument::pass_through` applies the normal pass-through behavior.

```cpp
const bg::WiringTemplate &runtime_graph = graph_template;
std::vector<bg::MapArgument> arguments{
    {bg::WiringArgument{values}},
    {bg::WiringArgument{factors}, true},
};

auto result = bg::map(runtime_graph, std::span<const bg::MapArgument>{arguments});
```

A runtime named argument stores its name in the `WiringArgument`. This includes
map options:

```cpp
arguments.push_back({bg::WiringArgument{
    bg::util::symbol{"__key_set_arg__"}, std::int64_t{0}}});
```

Generic C++ lambdas use the typed call because their call interface exists only
at compile time.

## Arguments

A TSD argument is mapped by default. The inner graph receives the item for its
key through a reference. With several mapped TSD arguments, their key sets form
a union. An item that is absent from one TSD produces an invalid input for that
inner graph.

`pass_through(argument)` passes one complete time series to every inner graph.
It excludes a TSD or TSS from key selection.

Scalar arguments are wired into the compiled inner graph. Named arguments keep
their names when the first argument is a graph template.

## Key set

Without an explicit key set, mapped TSD arguments supply the union of current
keys. A sole TSS argument supplies the key set and is not passed to the inner
graph. Calls with several TSS arguments must select one.

`"__key_set_arg__"_n` selects a TSD or TSS argument by flat index or name. A
selected TSD remains a mapped argument. Its key set determines which inner
graphs exist. A selected TSS is omitted from the inner graph arguments.
Unselected TSS arguments are passed through.

## Key argument

The key can enter the inner graph as `TS<K, const>`. A graph template receives
it when its first parameter is named `key` or when `"__key_arg__"_n` selects a
parameter by index or name.

A graph function receives a leading key when that is its only valid parameter
sequence. An ambiguous graph function must select the key parameter by index.
C++ graph functions do not expose parameter names.

## Output

A graph with output `R` produces `TSD<K, REF<R, deduplicated>>`. The references
identify the inner graph outputs without copying their values. A sink graph
produces no output. A source graph obtains its keys from a selected TSS.

`map` is an operator. Overloads can match the supplied graph structure and the
remaining arguments. Structurally equivalent graphs share overload-resolution
cache entries as defined by [operators](operators.md#graph-arguments).

## Evaluation and lifecycle

An added key starts a new inner runtime graph. Removing the key stops that
runtime graph and removes its output item. The stopped runtime graph remains
allocated through the current outer cycle and is destroyed at the start of the
next outer cycle.

A mapped item update evaluates only the inner runtime graph for that key. A
pass-through input update evaluates every current inner runtime graph. Immediate
and future scheduling follow the [nested graph rules](nested-graphs.md#evaluation).
