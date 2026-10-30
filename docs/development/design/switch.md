# Switch

## Purpose

`switch_(key, cases, arguments...)` runs one inner graph selected by the current
value of `key`. The key is a value time series and is not passed to a case graph.

```cpp
auto result = bg::switch_(
    key,
    bg::cases{
        bg::case_(1, first_graph),
        bg::case_(std::tuple{2, 3}, second_graph),
        bg::case_(bg::DEFAULT, fallback_graph),
    },
    input);
```

`cases` retains heterogeneous graph-template and graph-function types. A case
accepts one key or a non-empty tuple of keys. Tuple keys share one wired and
compiled graph definition, but each key has a separate selection identity.
Changing between tuple keys therefore starts a fresh graph instance. Case keys
use the key's scalar type. Keys must support equality and hashing, and duplicate
keys are rejected across the complete case set.

## Arguments

A graph-template parameter first consumes a supplied argument with the same
name. Its remaining fixed parameters consume positional arguments in order.
The graph can omit named arguments. Its variadic parameters capture remaining
arguments normally.

A graph function receives all supplied arguments in order. This supports
generic lambdas without an additional graph declaration.

`switch_` retains the case graph templates and graph functions used to wire its
branches through compilation. This is required to keep node templates captured
by value alive for the [node-template lifetime contract](graph-wiring.md#wiring-template-call).

Type-erased callers pass a key as a `WiringPoint` or `WiringArgument`, a span
of `SwitchCaseArgument`, and a span of `WiringArgument`. A qualified key keeps
its `opt` qualifier during overload resolution. A `SwitchCaseArgument` accepts
one key, a span of keys, or `DEFAULT`. It borrows its graph template; the caller
keeps that template and its referenced node templates alive through compilation.

## Selection and output

A valid matching key selects its case. `DEFAULT` handles an invalid or unmatched
key when present. Without `DEFAULT`, an invalid or unmatched key raises an
evaluation error.

Keyed cases must either be sink graphs or produce compatible output types. An
output switch publishes a reference to the active inner graph output. Its
`DEFAULT` case may be outputless; selecting that case invalidates the switch
output. A source case accepts no arguments. A sink switch produces no output.

Output inference removes references recursively from each branch target, then
computes their [common type](type-system.md#time-series-guarantees) `C`. The
switch publishes `REF<C>`. For example, branches returning `TSD<K, REF<T>>` and
`TSD<K, T>` produce `REF<TSD<K, T>>`. Consumers use the normal
[reference representations](time-series-references.md#dynamic-representations)
to request value or reference items. Free and bound collection storage does not
change output compatibility.

`switch_` is an operator. Its call preparation converts heterogeneous C++ case
definitions before normal overload resolution.

## Evaluation and lifecycle

An equal key update keeps the current inner graph. A different selected case
stops the current graph and starts a fresh instance of the selected graph.
Returning to an earlier case creates fresh state.

Other input changes, immediate inner scheduling, and timed inner scheduling
evaluate the active graph. A stopped graph remains allocated through the current
outer cycle and is destroyed before the next outer cycle, as required by
[nested graph lifecycle](nested-graphs.md#lifecycle).
