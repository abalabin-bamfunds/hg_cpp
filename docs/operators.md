# Operators

## Purpose

An operator gives graph code one stable name for behavior that can have several
node or graph implementations. Wiring selects the unique least-generic
applicable overload matching the supplied arguments.

## Declaration and registration

An operator has object identity, a fully qualified name, and a prototype. The
prototype describes intended use. It does not restrict overload parameter
names, parameter count, parameter types, or output type.

C++ uses the operator object as its identity.

Public headers declare an operator from a function type. The definition supplies
its name and parameter names:

```cpp
namespace bg::operators {

extern operator_<auto(TS_T, TS_T) -> TS_T> add;

} // namespace bg::operators

decltype(bg::operators::add) bg::operators::add{"add", {"lhs", "rhs"}};
```

Local code uses the same declaration form:

```cpp
auto add = bg::operator_<auto(bg::TS_T, bg::TS_T) -> bg::TS_T>{"add", {"lhs", "rhs"}};
add.overload(bg::node<add_integers>());
add.overload(add_numbers_graph);
```

An operator can declare ordered type parameters before its prototype. Calls can
then supply positional type arguments or named mappings:

```cpp
using RESULT = bg::TimeSeriesVariable<"RESULT">;

auto convert = bg::operator_<RESULT, auto(bg::TS_T) -> RESULT>{"convert", {"value"}};

convert[bg::type<bg::TS<bg::i64>>](value);
convert[bg::type<RESULT> = bg::type<bg::TS<bg::i64>>](value);
```

The optional template argument after the prototype defines C++ call preparation.
Its `invoke` function receives the explicit type map, the original arguments,
and a continuation that runs normal overload resolution. This supports
arguments, such as generic C++ lambdas, that must be converted while their
concrete C++ types are available. The preparation code does not select or
invoke an overload itself.

The prototype determines the C++ return type. An output operator returns `wp<>`
because overloads can produce different output types. A sink overload returns
an empty `wp<>`. An outputless operator returns `void` and accepts only sink
overloads.

Prototype parameter names are public keyword aliases for the overload
parameters at the same positions. An overload's own parameter name takes
precedence when it conflicts with a prototype name.

```cpp
auto result = add(lhs, rhs);
```

Engine operators are declared in `bg::operators`. Each time series or scalar
module registers the overloads it implements.

`const_(scalar)` lifts a scalar to `TS<scalar, const>`. Graph compilation stores
the scalar once and binds its consumers directly to that storage.

`default_(source, fallback)` adapts a `TS<scalar>` input. A value consumer reads
`fallback` while `source` is invalid and reads `source` otherwise. The adapter
wires a node with the source and a const fallback. Its template identifies the
two inputs to graph compilation. Compilation lowers each consumer input to a
direct input binding and omits the adapter runtime node. It returns an ordinary
time series because switching between `source` and `fallback` can add updates
independently of the source guarantee. A reference consumer uses a normal
reference node over the adapter output. Nested graph outputs therefore use the
reference path.

The C++ library declares `bg::operators::add` in `<bg/library.hpp>` and
registers overloads in the `bg::library` target. Its initial overloads add
`double` scalars and time series in all scalar/time-series combinations. The
result is a time series and retains the weakest time-series input guarantee.
Adding two scalars creates a constant graph source. The C++ wiring-point
`operator+` overloads are defined by the same library header.

An operator owns its registered overloads. Registration is permanent and is
not concurrent with graph wiring. Adding an overload clears that operator's
resolution cache. Existing wired graphs remain valid because the operator
retains every registered implementation.

Node templates and graph templates can both be overloads. Their normal wiring
behavior runs only after overload selection.

## Candidate resolution

The operator performs overload resolution within its wiring call.

For each candidate, resolution proceeds in this order:

1. Match its parameters against the supplied argument types.
2. Resolve its parameter and output types. This demands only mappings those types use.
3. Resolve any additional types needed by the wiring template's metadata.
4. Run the requirement while resolver lookup remains active.
5. Retain the candidate when each demand and its requirement succeeds.

Resolvers run only when signature resolution, template metadata, requirements, or
another resolver requests their variable. A resolver can request dependencies
through `TypeMap::find`. The resolver lookup scope closes before the candidate
is cached or wired. The completed type map retains mappings produced by the
candidate's demands.

A missing resolver or an invalid resolver result rejects a candidate when a
variable is demanded. A requirement rejection also rejects the candidate.
Type resolution errors propagated from a demanded resolver reject the
candidate. Other exceptions thrown by resolver or requirement callbacks are
wiring failures and stop overload resolution. A `TypeResolutionError` thrown by
the requirement itself is a callback failure.

Reference matching follows the transparent rules in
[the type system](type-system.md#signature-resolution). Matching retains the
original argument types. A requirement function can therefore distinguish a
reference argument from a direct argument when an implementation requires that
information.

Reference shape does not change type specificity. Selection first prefers a
reference parameter for a reference argument and a direct parameter for a
direct argument. The preference applies recursively to collection item types.
For example, `TSL<REF<V>, N>` is preferred over `TSL<V, N>` for a list of
references. It then compares the remaining type specificity. Conflicting
reference preferences across parameters remain ambiguous.

Free collection materialization and other wiring adaptations happen
after selection. This ensures that ranking describes the graph-code arguments,
not infrastructure introduced by wiring.

## Optional arguments

`opt(point)` requires the receiving parameter to have the `optional` input
validity constraint. It retains the time series type and identity. It adds no
node, supplies no fallback value, and does not inspect runtime validity or
schedule evaluation.

```cpp
operation(x, y);            // Any validity constraint is applicable.
operation(opt(x), y);       // The first parameter must be optional.
operation(opt(x), opt(y));  // Both parameters must be optional.
```

The selected implementation defines the result for an invalid optional input.
For example, addition can use zero and multiplication can use one. An operator
without an applicable optional-input overload reports a wiring error.

When applicable overloads have equivalent parameter types, `required` and
`all_valid` time series parameters are equally preferred over `optional` for
unmarked arguments. Otherwise equivalent `required` and `all_valid` overloads
remain ambiguous. One overload must be at least
as preferred at every parameter and strictly preferred at one. Conflicting
validity preferences remain ambiguous. Type specificity precedes this
preference. This preference applies only to time series parameters; scalar
absence keeps its existing matching rules.

Optional inputs retain their declared activity. To publish a result while every
input is initially invalid, an implementation must request
[`schedule_on_start`](node-api.md#plain-c-parameters) or schedule itself.

The default `map`, `mesh`, and `switch_` overloads accept qualified variadic
arguments. Each branch signature checks the qualifiers of the arguments it
receives.

Use `default_(point, fallback)` when every consumer should read the supplied
fallback while the point is invalid.

## Resolvers

A resolver supplies a variable that argument matching did not map. Resolvers
run on demand and can depend on other type map entries and wiring-time scalar
arguments. Resolution detects dependency cycles.

A type resolver returns a reference or pointer to a persistent semantic type
obtained from a declaration's `type()` function or a type registry.

Resolvers and requirements receive a read-only wiring-argument view. It
provides argument names and presence, original argument types, available
resolved types, the current type map, and wired scalar values. Arguments can be
addressed by index or name. Scalar access through this view records the reads
required by the resolution cache.

`WiringTemplate::resolve_additional_types` is the engine- and extension-facing
hook for type mappings needed by template metadata. It runs after signature
resolution and before the requirement, with resolver lookup active. A node
template uses it to demand state and dynamic-reference input types.

The view presents one flat argument sequence. Fixed arguments come first and
captured variadic arguments follow. Forwarding a variadic wiring record produces
the same sequence as supplying its arguments directly. An absent fixed scalar
keeps its position. An absent captured scalar is omitted with its pack entry.

Callable node declarations use a resolver named for its variable:

```cpp
using RESULT = bg::ScalarVariable<"RESULT">;

bg::node(
    bg::eval = evaluate,
    bg::resolve<RESULT> = resolve_result);
```

A schema variable uses `schema_type` because its TSB declaration is a template:

```cpp
bg::resolve<bg::schema_type<bg::SCHEMA>> = resolve_schema
```

An aggregate definition declares overloaded static functions. The variable
token identifies the result:

```cpp
struct implementation {
    using INPUT = bg::ScalarVariable<"INPUT">;
    using RESULT = bg::ScalarVariable<"RESULT">;

    bg::In<bg::TS<INPUT>> input;
    bg::Out<bg::TS<RESULT>> output;

    static const bg::ScalarType *resolve(RESULT, auto arguments);

    void evaluate() const;
};
```

Guarantee and size variables use their declaration types.

## Requirements

A requirement function checks scalar values or relationships that type
matching and resolvers cannot express. It runs only after the candidate has a
resolved signature. It can request auxiliary mappings through the wiring
argument view's type map. A requirement determines applicability but not
specificity.

Resolvers and requirements must determine their results only from the supplied
wiring arguments and type map. This is required for resolution caching.

A requirement can return `bool` or `RequirementResult`. Use `accept` and
`reject(message)` to construct the latter without spelling its representation:

```cpp
static auto require(auto arguments) -> bg::RequirementResult {
    if (!count_is_valid(arguments)) {
        return bg::reject("count must be non-negative");
    }

    return bg::accept;
}
```

Callable declarations use the same protocol through `bg::require`.

## Specificity

Applicable signatures form a partial order from more generic to less generic.
The operator selects a candidate only when it is strictly less generic than
every other applicable candidate. Otherwise the call is ambiguous.

Comparison includes parameter types, constraints, literal components, and
relationships created by repeated variables. It does not include output types,
parameter names, input activity, or requirement functions. Input validity
constraints break ties between equivalent parameter types as described in
[Optional arguments](#optional-arguments).
The implementation must compare complete signatures rather than add numeric
scores for individual parameters.

Specificity follows these rules:

1. An exact type is less generic than a variable that accepts it.
2. A constrained variable is less generic than an unconstrained variable when
   its accepted set is a strict subset.
3. A specific time series form is less generic than a time series variable.
4. Repeated use of one variable is less generic than independent variables.
5. Two signatures that constrain different parameter positions are unordered.
6. A literal fixed collection size is less generic than a size variable.
7. Applicable literal guarantees follow `const`, `final`, `deduplicated`, then
   `ordinary`; a literal is less generic than a guarantee variable.
8. A derived scalar type is less generic than an accepted base scalar type.
9. Unrelated capability constraints are unordered.
10. Identical signatures remain ambiguous when more than one requirement
    succeeds.

A fixed overload is preferred over a variadic overload. Among variadic
overloads, reference affinity and type specificity compare their shared fixed
parameters. A longer compatible fixed prefix is less generic.

## Graph arguments

A higher-order operation can receive a graph as an owning wiring argument.
Graph arguments do not enter node storage or variadic collections.

`GRAPH<auto(parameters...) -> result>` declares a graph parameter with a
boundary signature. `GRAPH_T` accepts any graph. Scalar and time series type
variables inside `GRAPH` use the overload's normal type map. Repeated variables
therefore constrain the complete graph signature and participate in
specificity.

```cpp
using namespace bg::literals;

using VALUE = bg::ScalarVariable<"VALUE">;

auto implementation = bg::graph(
    "name"_n = "same_type_graph",
    [](bg::GRAPH<auto(bg::TS<VALUE>, bg::TS<VALUE>) -> bg::TS<VALUE>> function) {
        // ...
    });
```

An overload parameter can accept any graph, one exact graph structure, or a graph pattern.
Pattern matching wires the pattern against the supplied graph's ordered boundary
types and compares the resulting structure. Exact structures and patterns are
less generic than an otherwise equivalent unconstrained parameter. Distinct
structural constraints are unordered.

`graph_pattern(function)` declares a structural constraint. Signature
construction attaches it to the parameter declared with `GRAPH`:

```cpp
inline constexpr auto addition = bg::graph_pattern(
    [](auto lhs, auto rhs) {
        return bg::operators::add(lhs, rhs);
    });

using ADDITION = bg::GRAPH<
    auto(bg::TS<VALUE>, bg::TS<VALUE>) -> bg::TS<VALUE>,
    addition>;
```

The [`map` operator](map.md) passes the mapped graph as a graph argument. An
overload can therefore select an implementation from the graph structure and
the supplied map arguments without compiling a separate inner graph for each
key.

The [`switch_` operator](switch.md) prepares its heterogeneous case definitions
before normal overload resolution.

## Resolution cache

Each operator has a bounded resolution cache. Its keys include supplied
argument types, presence, and `opt` qualifiers, explicit type mappings, and
only the wiring-time scalar values read by a resolver or requirement function.
Qualifiers inside forwarded variadic records also contribute to the key. Reads
made by rejected candidates are part of the key when they can affect the selected
result. The key also includes the presence, type, and qualifier of every implicit
context value that can affect overload selection. Cache validation rebuilds the flat
argument sequence for the candidate that made each read.

Graph argument keys retain the stable signature type and owned graph structures
separately. Equal structures share a cache entry; stored scalar values and the
order of multiple structures participate in equality. Keys retain no argument
payload. A graph structure that contains a value without equality or hashing
bypasses the cache.

Resolution is not cached when a read scalar does not provide equality and
hashing. Cache entries retain the selected operator-owned implementation and
the completed type map.

## Diagnostics

Candidate mismatch and rejection are expected results. Operator resolution
collects each rejected candidate's implementation identity, signature,
rejection stage, and first decisive reason.

Type mismatch data includes the parameter name and the expected and supplied
types. Resolver failure data identifies the variable and whether its resolver
was missing or returned an incompatible result. Requirement failure data
includes its optional message.

The public wiring boundary throws `WiringError` when no unique overload exists.
The error owns the structured diagnostic and also renders readable text. This
lets callers inspect the failure directly and lets language bindings expose
fields without parsing `what()`.

A no-match diagnostic reports the operator name, wiring path, supplied
arguments, and each candidate's first decisive failure. An ambiguity diagnostic
reports the applicable candidates and states that none is uniquely least
generic. Exceptions from callbacks retain the operator, candidate, wiring path,
and callback stage.

Materialisation diagnostics distinguish the forms offered by an argument from
the forms in a matched argument binding. Candidate diagnostics include form
costs and, for a completed binding, its total materialisation cost. Failed
bindings retain their decisive rejection reason; ambiguous bindings retain the
competing forms. Reporting uses collected resolution results and does not rerun
requirements or materialisation callbacks.

Errors raised inside deferred wiring retain these diagnostic fields and add
the [requesting context](graph-wiring.md#wiring-errors).

```text
Cannot resolve operator add at portfolio/pricing/spread
Arguments: (lhs: TS<i64>, rhs: string)

add_numbers(lhs: TS<Number>, rhs: TS<Number>)
  rhs: expected TS<Number>, received string

add_dynamic(lhs: TS<T>, rhs: T)
  requirement rejected the arguments: rhs must be numeric
```
