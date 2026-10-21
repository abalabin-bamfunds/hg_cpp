# Operators and overloads

An operator is a named graph callable with registered implementations. Use it
when graph code should choose a node or graph implementation from supplied
types. Use a direct node for one concrete calculation.

## Resolution

A call binds positional and named arguments, resolves type variables, checks
constraints, and selects a compatible overload. Exact scalar types matter;
integer and floating-point operands do not implicitly mix. Required and
optional parameter contracts also affect selection. A fixed-parameter overload
is preferred over a variadic overload before type specificity is compared.

Missing implementations, incompatible arguments, and ambiguous matches are
construction errors. A failure reports the operator, argument types, candidate
signatures, and rejection context. See [errors](../how-to/errors.md).

## Extension

C++ declares an `Operator` from a prototype and registers node or graph templates
with `Overload`. Registration objects and callable definitions must outlive all
uses. Type parameters, requirements, and resolvers refine signatures before
construction. Keep overload contracts consistent with the operator's purpose.

Python `@operator` and `@overload` expose typed Python prototypes and graph/node
implementations where supported. Native standard-library operators remain
objects backed by the shared C++ overload system. A Python wrapper can observe
native overloads registered later. C++ extension modules can export an existing
operator through `bg::python::export_operator` without modifying a central list.

See [C++ integration](../how-to/cpp-extension.md) for build and ownership rules.
Do not register a conversion overload merely to hide an incompatible numeric
expression; explicit `convert` makes the resulting value type visible.
