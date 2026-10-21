# Operators

## Purpose

An operator gives graph code one stable name for behavior that can have several
node or graph implementations. Wiring selects the unique most-specific
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

extern operator_<auto(SCALAR, SCALAR) -> SCALAR> add;

} // namespace bg::operators

decltype(bg::operators::add) bg::operators::add{"add", {"lhs", "rhs"}};
```

Local code uses the same declaration form:

```cpp
auto add = bg::operator_<auto(bg::SCALAR, bg::SCALAR) -> bg::SCALAR>{"add", {"lhs", "rhs"}};
add.overload(bg::node<add_integers>());
add.overload(add_numbers_graph);
```

A scalar result `T` in an operator prototype means `TS<T>`. Native scalar
results use their canonical type; for example, `int` means `TS<i64>`.
The selected overload determines the actual output type and guarantee.
Explicit time series results and `void` retain their normal meaning.

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
overloads. Use `has_output()` to test whether the returned `wp<>` identifies an
output.

Prototype parameter names are public keyword aliases for the overload
parameters at the same positions. An overload's own parameter name takes
precedence when it conflicts with a prototype name.

```cpp
auto result = add(lhs, rhs);
```

Including `<bg/library.hpp>` and linking `bg::library` provides the common
operator surface. It includes engine operators such as `const_`, `default_`,
and `getitem`, alongside C++ library operators. Their implementation
placement does not create a separate operator API for callers.
Each time series or scalar module registers the overloads it implements.

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

The [standard library](library.md) defines supported operators, scalar types,
optional behavior, and C++ and Python wiring syntax.

An operator owns its registered overloads. Registration is permanent and is
not concurrent with graph wiring. Adding an overload clears that operator's
resolution cache. Existing wired graphs remain valid because the operator
retains every registered implementation.

Node templates and graph templates can both be overloads. Their normal wiring
behavior runs only after overload selection.

## Overload implementation declarations

`<bg/overload.hpp>` provides declarations for library initialization:

- `overload<OP>{arguments...}` forwards the arguments to `node()` and registers
  the resulting node template with `OP`.
- `overload<OP>{implementation}` registers an existing node or graph template
  directly. Use `graph(...)` explicitly for graph functions.
- `overload<OP, Definition>{}` constructs and registers `node<Definition>()`.
- `instantiate_overloads<OP, Definition, T...>{}` constructs and registers
  `node<Definition<T>>()` for each listed type. `Definition` is a C++ class
  template; the list selects separate specializations, not type combinations.

These initialization forms register synchronously. The operator owns the
registered template. The declaration object retains borrowed pointers so a
registration group can assign its reflected member name.

Place these declarations as ordinary members of a file-local class. Keep each
aggregate definition beside its registration member and group members by
operator:

```cpp
struct numeric_overloads {
    bg::overload<bg::operators::add> add_double{
        [](double lhs, double rhs) { return lhs + rhs; }
    };

    bg::overload<bg::operators::add> add_optional_double{
        [](std::optional<double> lhs, std::optional<double> rhs) {
            return lhs.value_or(0.0) + rhs.value_or(0.0);
        }
    };
};

void register_numeric_operators() {
    bg::register_overloads<numeric_overloads>();
}
```

`register_overloads<Group>()` constructs the group, then assigns each reflected
member name to its registered node or graph template. Every member must be an
`overload` or `instantiate_overloads` declaration. A class-template family uses
the member name for each specialization; their signatures distinguish them.

Name a default overload `<operator>_impl` and other overloads
`<operator>_<what>`, for example `sample_impl`, `add_double`, and `len_tss`.
Remove a trailing underscore from the operator name before adding the suffix:
`filter_impl`, not `filter__impl`. Use the same rule for implementation aggregate
names.

Library initialization calls the file's registration function after constructing
the operator objects. Members register in declaration order. Split groups larger
than the aggregate reflection limit into related operator groups. The temporary
group adds no runtime node storage. Declaring the class alone performs no
registration. Standalone registration declarations retain their template's
inferred or explicitly supplied name.

## Candidate resolution

The operator performs overload resolution within its wiring call.

For each candidate, resolution proceeds in this order:

1. Validate argument qualifiers and scalar presence, then match the supplied types.
2. Resolve its parameter and output types. This demands only mappings those types use.
3. Resolve any additional types needed by the wiring template's metadata.
4. Run the requirement while resolver lookup remains active.
5. Retain the candidate when each demand and its requirement succeeds.

An explicit operator type argument makes an overload applicable when its
signature, additional type resolution, or requirement uses that variable.
A concrete overload also accepts a variable used directly as a scalar prototype
parameter when its corresponding parameter has the requested type. Other
overloads reject the explicit argument even when their inferred output matches.

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

Matching considers each advertised materialisation form and each compatible
Union alternative. It selects the lowest-cost binding before comparing type
specificity. A top-level reference adaptation costs `1`; a change between
reference and value items inside one collection input costs `2`, once for that
input rather than once per item. An advertised materialisation contributes its
declared cost once per source and target. Equal-cost bindings with distinct
observable type mappings remain ambiguous.

Graph overloads with Union parameters receive the original wiring point and
can select its form inside the graph body. Node overloads receive the selected
concrete form. Materialisation runs only after selection.

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
remain ambiguous.

For a supplied scalar value, a parameter that requires presence is preferred
over an otherwise equivalent parameter that accepts absence. This applies to
scalar-only declarations and scalar-or-time-series parameters resolved as
scalars. An absent scalar rejects a parameter that requires presence before
overload ranking. A fixed scalar parameter marked `accepts_absent_scalar` can be
omitted. Wiring supplies a typed absence, including when a later named argument
skips that parameter. Omission requires one concrete scalar type in the
parameter declaration; unresolved generic parameters and parameters with
multiple scalar alternatives must be supplied explicitly.

One overload must be at least as preferred at every parameter and strictly
preferred at one. Conflicting preferences remain ambiguous, including conflicts
between scalar presence and time series validity. Type specificity precedes
these preferences.

Optional inputs retain their declared activity. To publish a result while every
input is initially invalid, an implementation must request
[`schedule_on_start`](node-api.md#plain-c-parameters) or schedule itself.

The default `map`, `mesh`, and `switch_` overloads accept qualified variadic
arguments. Each branch signature checks the qualifiers of the arguments it
receives.

Use `default_(point, fallback)` when every consumer should read the supplied
fallback while the point is invalid.

## Valid-item arguments

`valid_items(point)` requires a receiving parameter with the `valid_items`
input guarantee. The selected input view filters
[immediate TSD items](time-series-validity.md#collections); the annotation does
not modify the time series or create a filtered collection.

For unmarked arguments, an otherwise equivalent parameter without
`valid_items` is preferred. This preference combines with input validity and
scalar presence under the same rule: the selected overload must be at least as
preferred at every parameter and strictly preferred at one. Conflicting
preferences remain ambiguous.

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
required by the resolution cache. `resolved_output_type()` returns the resolved
time-series output type, or `nullptr` for a sink signature.

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

The operator compares applicable signatures as declarations. It selects a
candidate only when it is more specific than every other applicable candidate.
Otherwise the call is ambiguous.

Comparison includes parameter types, constraints, literal components, and
relationships created by repeated variables. It does not include output types,
parameter names, input activity, or requirement functions. Time series validity
and scalar presence requirements break ties between equivalent parameter types
as described in [Optional arguments](#optional-arguments).
The implementation must compare complete signatures rather than add numeric
scores for individual parameters.

Comparison recursively applies one rule: a declaration is more specific when
it narrows the accepted types or variable relationships, with no conflicting
preference elsewhere in the signature. An exact type narrows a variable; adding
a capability constraint narrows an unconstrained variable. Collection items and
graph boundaries use the same comparison. Repeated variables, literal sizes,
literal guarantees, and derived types also narrow their corresponding generic
or base declarations. Applicable literal guarantees follow `const`, `final`,
`deduplicated`, then `ordinary`.

Union comparison uses each candidate's supplied argument types to identify its
applicable alternatives. It compares those alternatives as declarations, not
as their resolved types. Conflicting applicable alternatives remain unordered.
When applicable alternatives compare equivalent, declared containment still
distinguishes an exact declaration from a union containing it. Partially
overlapping concrete unions remain unordered.

For a supplied `TS<i64>`, with equivalent guarantees and variable relationships,
the declarations have this order, from most to least specific:

```cpp
TS<i64>
Union<"VALUE", i64, TS<i64>>
TS<EQUALITY_SCALAR> // ScalarVariable<"EQUALITY_SCALAR", EqualityComparable>
TS<SCALAR>
```

Constraints on different parameters can conflict. Unrelated capability
constraints and identical signatures do not establish a strict preference.
For supplied `(i64, i64)`, `(i64, R)` and `(T, T)` remain unordered: the first
fixes one parameter type; the second requires both parameter types to match.
Declaration-only comparisons remain conservative when no supplied types are
available to identify applicable union alternatives.

A fixed overload is preferred over a variadic overload. Among variadic
overloads, reference affinity and type specificity compare their shared fixed
parameters. A longer compatible fixed prefix is more specific.

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

Root `GraphWiring` owns a bounded resolution cache for each invoked operator.
Nested wiring shares these caches through its parent wiring. Independent roots
resolve independently. Destroying the root releases cached scalar values and
graph structures. Registering an overload invalidates cached resolutions for
that operator in every root.

Cache keys include supplied argument types, presence, `opt` and `valid_items`
annotations, explicit type mappings, and only the wiring-time scalar values
read by a resolver or requirement function. Free TSB fields contribute their
original argument categories and annotations. Annotations inside forwarded
variadic records also contribute to the key. Reads
made by rejected candidates are part of the key when they can affect the selected
result. The key also includes the presence, type, and annotations of every implicit
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
