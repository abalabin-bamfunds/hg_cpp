# Operators and overloads

An operator is a named graph callable with registered implementations. Use it when graph
code should choose a node or graph implementation from supplied types. Use a direct node
for one concrete calculation.

## Resolution

A call binds positional and named arguments, resolves type variables, checks
constraints, and selects a compatible overload. Exact scalar types matter; integer and
floating-point operands do not implicitly mix. Required and optional parameter contracts
also affect selection. A fixed-parameter overload is preferred over a variadic overload
before type specificity is compared.

Missing implementations, incompatible arguments, and ambiguous matches are construction
errors. A failure reports the operator, argument types, candidate signatures, and
rejection context. See [errors](../how-to/errors.md).

## Extension

C++ declares an operator prototype with `operator_` and registers implementations with
`overload`:

```cpp
#include <bg/library.hpp>

using namespace bg;

inline operator_<auto(i64) -> i64> doubled{"doubled", {"value"}};
inline overload<doubled> implementation{[](i64 value) { return value * 2; }};
```

`overload<operation, Definition>` registers an aggregate node definition.
`overload<operation>{graph_template}` registers a graph implementation.
`register_overloads<Group>()` constructs a group of overload members. Registration
retains its implementation after the registration object leaves scope; the operator
object must remain alive for its uses. Type parameters, requirements, and resolvers
refine signatures before construction.

Python currently exposes native operators rather than Python `@operator` or `@overload`
declarations. A Python wrapper can observe native overloads registered later. C++
extension modules export an operator through `bg::python::export_operator`.

See [C++ integration](../how-to/cpp-extension.md) for build and ownership rules. Do not
register a conversion overload merely to hide an incompatible numeric expression;
explicit `convert` makes the resulting value type visible.
