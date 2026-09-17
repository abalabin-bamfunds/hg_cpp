# Variadic wiring

Status: accepted design, not implemented.

Variadic parameters collect wiring-time arguments into a wiring record. The
record preserves argument order, names, and whether each argument was supplied
as a scalar or time series.

A graph declares positional or named capture through a `wp` property:

```cpp
graph([](wp<TSB<SCHEMA>, var_kwargs> arguments) {
    auto argument = arguments["window"];
    if (argument.is_scalar()) {
        auto window = argument.scalar();
    }
});
```

Graph code can access an argument by position or name, inspect its `Type`, and
obtain either its scalar value or wiring point. Scalar extraction is available
only during graph wiring.

The wiring record has a complete TSB projection. A supplied time series keeps
its type. A scalar of type `S` becomes `TS<S, const_>`. `SCHEMA` resolves to the
schema of this normalized TSB. An absent optional scalar is omitted. Wiring
rejects a scalar that cannot be stored in a time series.

Passing the record to another variadic parameter preserves the original
arguments. Passing it to an ordinary TSB parameter materializes the normalized
TSB.

A node can use the corresponding variadic input declaration:

```cpp
node([](In<TSB<SCHEMA>, var_kwargs> arguments) {
    // Every field is a time series here.
});
```

The node receives one TSB input and sees every field as a time series.
Materialization creates one free TSB node for the complete pack. Time-series
fields alias their inputs. Scalar fields use the node's scalar arguments and
appear as const time series with a shared start timestamp.
