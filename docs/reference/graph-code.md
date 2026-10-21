# Graph declarations and calls

Graph code constructs connections between calculations. It runs during graph
construction; node callbacks run during evaluation. Use graph functions to name and
compose calculations without creating a runtime graph boundary.

## Declarations

| Purpose | Python | C++ |
| --- | --- | --- |
| Composable graph | `@graph` | Ordinary graph function |
| Checked, named graph boundary | `@graph` | `graph(callable)` |
| Changing integer input | Plain `int` or explicit `TS[int]` | `wp<TS<i64>>` |
| Scalar-only input | `Scalar[int]` | Scalar parameter on a checked graph template |
| Inferred output | Plain scalar return annotation declares `TS` | Inferred callable return |
| No output | `-> None` | `void` |

Python plain graph annotations accept fixed scalars or matching `TS` connections.
Explicit time series annotations accept time series only. An explicit `TS` return
requires a connection; a plain scalar return can also lift a returned scalar through
`const_`. C++ plain functions receive exactly the arguments from their callers. Use
`wp<>` parameters when their inputs must be connections.

A graph call expands its body into the current graph. Equal output-producing node calls
normally reuse one wired node when their definition, resolved types, inputs, state
types, and scalar arguments match. Outputless calls are not reused. Sharing an existing
connection reuses that connection. `map`, `mesh`, and `switch_` create runtime inner
graphs; an ordinary graph call does not.

## Arguments

Python signatures follow positional, named, positional-only, and keyword-only parameter
rules. C++ named parameters use `"name"_n = argument`; import `bg::literals` for `_n`.
Checked C++ declarations can label parameters with `wp<TS<i64>, "reading"_n>` or provide
parameter names when declaring a template.

A scalar argument is fixed during construction. A time series argument supplies a
connection whose current value can change. A scalar can be lifted to a constant time
series where the receiving signature permits it.

## Optional arguments

`opt(point)` requests an optional receiving parameter. It creates no node and supplies
no fallback. It leaves the original connection unchanged. A receiver that requires a
current value rejects that request.

A plain optional parameter accepts an absent scalar or an optional time series input.
Node code receives absence when no value exists. Graph code receives the supplied scalar
or optional connection. See [nodes](nodes.md).

`valid_items(point)` similarly requires an accepting filtered dictionary input. See
[collection input views](input-views.md#collection-views).

## Context inputs

C++ `In<TS_T, "name"_n, context>` requests a named time series. A graph parameter can
request the same binding through `wp<TS_T, "name"_n, context>`. `with_context("name"_n =
point, callable)` establishes a lexical binding while constructing the callable's graph.
The nearest binding wins. A named argument supplied directly to the receiving call takes
priority over the lexical binding; a context parameter cannot bind a positional
argument.

Scalar bindings become constant time series. Missing names and incompatible bound types
fail during construction. A context binding is a graph dependency, not mutable shared
scalar storage. Python currently uses explicit arguments. See the [shared-input
example](../guide/shared-inputs.md).

## Generic signatures

Python `TypeVar("VALUE")` declares a scalar variable. Use it in `TS[VALUE]` or a plain
scalar signature. Bind a TypeVar to `TimeSeries` for a complete time series type.
`definition[TS[int], ...]` specializes a generic graph by its resolved input types, as
required by `test_graph`.

C++ `ScalarVariable<"VALUE">` declares a scalar variable and
`TimeSeriesVariable<"SERIES">` declares a complete time series variable. Explicit type
mappings use `type<VARIABLE> = type<ConcreteType>` on parameterized APIs. Repeated
occurrences share one mapping. Resolution fails when no compatible mapping or operator
overload exists.

## Variadic parameters

Variadic declarations capture arguments into one collection. Python bodies receive that
collection, rather than normal expanded `args` or `kwargs`.

| Capture | Python annotation | C++ declaration |
| --- | --- | --- |
| Homogeneous positional inputs | `*values: TSL[TS[int], SIZE]` | `wp<TSL<TS<i64>, SIZE>, var_args>` |
| Heterogeneous positional inputs | `*values: TSB[TS_SCHEMA]` | `wp<TSB<SCHEMA>, var_args>` |
| Named inputs | `**values: TSB[TS_SCHEMA]` | `wp<TSB<SCHEMA>, var_kwargs>` |

TSL capture requires matching time series item types. TSB capture permits scalars, which
become const time series, and uses positions or names as fields. Supplying a prebuilt
collection by the variadic parameter's name binds the whole collection. Node callbacks
receive a materialized collection view. Graph code receives wiring-time collection
access.

## Connection operations

Arithmetic and comparisons construct graph operations. A connection cannot be converted
to a native Boolean. Use graph Boolean operators and `if_then_else` for changing
conditions. `point.has_output` in Python or `point.has_output()` in C++ tests output
presence during construction, not a current runtime value.

C++ `.as<TS<T>>()` supplies a checked type for an untyped connection. A checked
graph boundary needs a concrete output type; use `.as` when its return expression
otherwise retains an unresolved type variable. It does not perform a scalar conversion. Use [`convert`](library-numeric.md#convert) for that.
Python connections expire when construction returns; they are not runtime input views.
See [errors](../how-to/errors.md) for signature and overload failures.
