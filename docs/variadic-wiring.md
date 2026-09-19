# Variadic wiring

Variadic parameters collect wiring-time arguments into a wiring record. The
record preserves argument order, names, and whether each argument was supplied
as a scalar or time series.

A signature can have one variadic parameter. It follows all fixed parameters
and uses either `var_args` or `var_kwargs`. `var_args` captures the remaining
positional arguments. `var_kwargs` captures named arguments written with
`"name"_n = value`. Duplicate names are rejected.

A graph declares positional or named capture through a `wp` property:

```cpp
graph("name"_n = "configure", [](wp<TSB<SCHEMA>, var_kwargs> arguments) {
    auto argument = arguments["window"];
    if (argument.is_scalar()) {
        auto window = argument.scalar();
    }
});
```

Graph code can access an argument by position or name, inspect its `Type`, and
obtain either its scalar value or wiring point. Scalar extraction is available
only during graph wiring.

```cpp
arguments.size();
arguments[0];
arguments["window"];
arguments.find("window");

auto argument = arguments[0];
argument.name();
argument.type();
argument.is_scalar();
argument.is_time_series();
argument.scalar();
argument.time_series();
```

Subscript access throws when the argument is absent. `find()` returns an optional
result. A selected argument is itself a wiring argument, so graph code can pass
`arguments[0]` directly to a node, graph, or operator.

The wiring record has a complete TSB projection. A supplied time series keeps
its type. A scalar of type `S` becomes `TS<S, const_>`. `SCHEMA` resolves to the
schema of this normalized TSB. An absent optional scalar is omitted. Wiring
rejects a scalar that cannot be stored in a time series.

A `TSL<TS_T, SIZE>` parameter with `var_args` captures homogeneous time series.
It rejects scalars and resolves `SIZE` from the capture count. An empty capture
requires a concrete item type and collection guarantee so that wiring can
resolve the list type.

A single TSL or TSB argument supplied with the variadic parameter's exact name
binds as one collection. Its items remain available as individual arguments to
resolvers and requirements. Item and field selection stays lazy. Otherwise the
argument follows the parameter's ordinary capture rules.

Passing the record to another variadic parameter preserves the original
arguments. A forwarded record cannot be combined with additional captured
arguments. Passing it to an ordinary TSB parameter materializes the normalized
TSB. Positional fields use their decimal positions as names in that TSB schema.

Resolvers and requirements see fixed arguments followed by individual captured
arguments. They do not see the TSB wiring record. Positional captures use their
flat argument indices as names. Named captures cannot reuse a fixed parameter
name.

A node can use the corresponding variadic input declaration:

```cpp
node([](In<TSB<SCHEMA>, var_kwargs> arguments) {
    // Every field is a time series here.
});
```

The node receives one TSB input and sees every field as a time series.
Materialization creates one free TSB node for the complete pack. Time-series
fields alias their inputs. Scalar fields materialize through `const_` and appear
as const time series with the graph's shared start timestamp. An empty pack
materializes as a valid empty TSB at graph start.

An empty variadic TSL also becomes valid when its materialization node starts.

A nested graph preserves the pack across its boundary. Each captured time
series crosses as a reference. Each captured scalar becomes immutable data in
the inner compiled graph. A directly bound collection crosses as one reference.

A variadic graph or node can be an operator overload. A matching fixed-parameter
overload is preferred over a variadic overload before type specificity is
compared.
