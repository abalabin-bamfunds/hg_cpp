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

The prototype determines whether the operator produces an output. Every
overload must have the same output presence as the prototype. An output operator
returns `wp<>` because overloads can produce different output types. An
outputless operator returns `void`.

```cpp
auto result = add(lhs, rhs);
```

Engine operators are declared in `bg::operators`. Each time series or scalar
module registers the overloads it implements.

`const_(scalar)` lifts a scalar to `TS<scalar, const>`. Graph compilation stores
the scalar once and binds its consumers directly to that storage.

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
2. Run resolvers for variables that matching did not map.
3. Resolve the complete signature and validate every resolver result.
4. Run its requirement function.
5. Retain the candidate when all preceding steps succeed.

A missing resolver or an invalid resolver result rejects a candidate when a
variable remains unresolved. A requirement rejection also rejects the
candidate. An exception from a resolver or requirement function is a wiring
failure and stops overload resolution.

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
resolved signature. A requirement determines applicability but not
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
parameter names, input activity, input validity, or requirement functions.
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

## Resolution cache

Each operator has a bounded resolution cache. Its keys include supplied
argument types and presence, plus only the wiring-time scalar values read by a
resolver or requirement function. Reads made by rejected candidates are part
of the key when they can affect the selected result. Cache validation rebuilds
the flat argument sequence for the candidate that made each read.

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

```text
Cannot resolve operator add at portfolio/pricing/spread
Arguments: (lhs: TS<int>, rhs: string)

add_numbers(lhs: TS<Number>, rhs: TS<Number>)
  rhs: expected TS<Number>, received string

add_dynamic(lhs: TS<T>, rhs: T)
  requirement rejected the arguments: rhs must be numeric
```
