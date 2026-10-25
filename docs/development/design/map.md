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

A generic C++ lambda uses the compile-time reference wrapper declared by the
map call. That wrapper can declare `ordinary` even when the actual inner graph
boundary has a stronger reference guarantee. For example, a sole mapped
`TSD<K, TS<i64, final>>` has an actual boundary of
`REF<TS<i64, final>, const>`, while the generic wrapper remains
`wp<REF<TS<i64, final>, ordinary>>`. An explicitly typed graph template sees
the actual boundary. Target and reference guarantees remain independent, so an
`In<TS<i64, G>>` resolves `G` from the dereferenced boundary type, whose
target guarantee is limited by the reference guarantee. For example, a union
of mapped `TS<i64, final>` values has an ordinary target guarantee even when
one source has a constant reference.

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

A mapped TSD controls branch lifetime when it is the only mapped TSD and no
key set is selected, or when it is the selected TSD key set. Its item reference
is constant for that branch. Other mapped TSD item references remain mutable
because their items can disappear and return while another input keeps the
branch alive. A selected TSS controls branch lifetime but does not make mapped
TSD item references constant. A reference to the root TSD also limits item
reference guarantees according to its reference guarantee. An explicit item
reference in `TSD<K, REF<TS_T>>` also caps the generated item reference
guarantee.

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

A non-reference graph output `R` produces `TSD<K, REF<R, const>>`. Each reference
identifies the fixed output handle of one inner graph, even when its target is
initially invalid. An explicit reference output preserves its reference
guarantee. A sink graph produces no output. A source graph obtains its keys
from a selected TSS.

`map` is an operator. Overloads can match the supplied graph structure and the
remaining arguments. Structurally equivalent graphs share overload-resolution
cache entries as defined by [operators](operators.md#graph-arguments).

## Evaluation and lifecycle

An added key starts a new inner runtime graph. Removing the key stops that
runtime graph and removes its output item. The stopped runtime graph remains
allocated through the current outer cycle and is destroyed at the start of the
next outer cycle. Starting an inner graph publishes its key input and schedules
active consumers of that key.

A mapped item update schedules only the inner runtime graph for that key.
Pass-through inputs use normal input subscriptions. Their consumers determine
which updates schedule an inner graph, including
[selected-key membership subscriptions](time-series-references.md#collection-membership-subscriptions).
Immediate and timed scheduling follow the [nested graph rules](nested-graphs.md#evaluation).
