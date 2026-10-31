# Type lifetime and graph argument API

## Purpose

Keep retained type pointers valid after graph destruction without keeping each
graph argument's captured values for process lifetime. The parameter constraint
split and registration migration are implemented in `.lanes/wiring-materialisation`;
work remains uncommitted. Python-enabled verification passes.

## Registry contract

Every `Type` belongs to process-owned registry storage, including primitive,
generic, variable, graph, extension, and Python declaration types. Static
declaration helpers cache references to registered objects. Generic declarations
retain the identities of their referenced variables; registering two independent
variables with the same name must preserve their independence.

Registration paths cover generic types, variable declarations, static declaration
helpers, Python metadata, primitives, and extension types. Existing resolved type
factories keep their canonical identities.

### Registration API responsibilities

The registration entry points return stable typed references:

```cpp
TypeRegistry::register_static(const TYPE&) noexcept -> const TYPE&;
TypeRegistry::instance().register_owned(std::unique_ptr<TYPE>) -> const TYPE&;
VariableRegistry::register_static(const VARIABLE&) noexcept -> const VARIABLE&;
VariableRegistry::instance().register_owned(std::unique_ptr<VARIABLE>) -> const VARIABLE&;
```

The variable entry point serves non-type metadata such as guarantee and size
variables. Type variables use the type entry point. Existing category factories
continue to select their canonical objects using their current keys. Registration
owns the objects selected by those factories; it does not deduplicate independent
declarations by name. Static registration requires process-static storage and
uses an entry embedded in the object. Type and variable objects cannot be copied
or moved. No caller-held registration token is required.

| API responsibility | Required behavior |
| --- | --- |
| Register a declaration | Retain a type and its metadata for process lifetime. Preserve a distinct variable declaration's identity. |
| Intern a composed type | Use the category's existing identity key and return its canonical registered object. |
| Retain referenced variables | Keep guarantee and size variables alive for every registered type that refers to them. |
| Register an extension type | Provide the same lifetime contract through an extension-facing API. |
| Complete a registration transaction | Preserve existing schema validation, recursive registration, and rollback of failed cache entries. |

These responsibilities apply to semantic types. Scalar and time series runtime
type registration keeps its existing API and ownership.

Built-in static types use allocation-free registration. Primitive type accessors
preserve their existing `noexcept` declarations. Dynamic registration accepts
ownership and can allocate.

## Ownership

```cpp
GraphArgumentType {
    parameter_types;
    result_type;
}; // process lifetime; contains no graph values

GraphArgument {
    const GraphArgumentType* type;
    one_structure_or_ordered_structures;
    payload;
}; // owns the argument's structures and payload

GraphArgumentConstraint {
    exact_structures_or_pattern;
};

Parameter {
    const Type* type;
    shared_ptr<const GraphArgumentConstraint> graph_constraint;
    // Existing parameter properties.
}; // the signature owns its constraint
```

An operator cache may retain argument structures in its existing bounded cache.
It does not retain the argument payload. Destroying the operator releases its
cached structures and overload constraints.

## BG API changes

| API | Change |
| --- | --- |
| `graph_argument_type(parameters, output)` | Return a process-owned signature type. Accept process-lifetime generic components as well as resolved components. |
| `GraphArgumentType` | Remove value structures and patterns. Match and resolve the boundary signature. |
| `GraphArgument` | Own its structures independently of its type. Keep `exact_type()`, `structure()`, and payload access. Remove `exact_type_ptr()`. |
| `GraphArgumentConstraint` | Own an exact structural constraint or pattern. Check supplied graph structures and compare constraint equivalence. |
| `Parameter::graph_constraint` | Retain an optional structural constraint independently of `type`. |
| Typed graph facade validation | Read constraints from the declaring signature alongside resolved parameter types. `ResolvedParameter` needs no added constraint storage. |
| `GRAPH<Prototype, Pattern>` | Keep existing graph-author syntax. `type()` describes the boundary; `constraint()` supplies structural metadata to signature construction. |
| `Signature` wiring matching | Match types and structural constraints before selecting an overload or invoking its graph function. |
| Signature specificity | Compare parameter structural constraints together with their types. A constrained parameter is more specific than an otherwise equivalent unconstrained parameter. Distinct constraints remain unordered. |
| Operator cache | Keep the stable type pointer and owned argument structures separately. Preserve ordered multiple structures, captured scalar values, and cache bypass for unhashable values. |
| Wiring diagnostics | Retain process-lifetime type pointers. Remove separate graph type ownership and duplicate type-description snapshots. |
| `generic_time_series_reference_type(target, guarantee)` | Return a process-owned generic reference declaration for `nested()`. Preserve generic matching before resolution. |

These low-level APIs serve engine and extension implementations. Graph authors
continue to use `GRAPH`, graph functions, and operators.

## Declaration examples

Graph-author syntax stays the same:

```cpp
using ADDITION = GRAPH<auto(TS<VALUE>, TS<VALUE>) -> TS<VALUE>, addition>;
auto implementation = graph("name"_n = "apply", [](ADDITION function) {
    // Use the supplied graph argument.
});
```

A custom wiring template attaches an exact constraint to its parameter:

```cpp
auto constraint = std::make_shared<const GraphArgumentConstraint>(argument.structure());
auto parameter = Parameter{
    .name = util::symbol{"function"},
    .type = &argument.exact_type(),
    .graph_constraint = std::move(constraint),
};
```

The parameter's type remains valid after both the argument and overload are
destroyed. Its constraint and captured values remain owned by the signature.

## Python migration

Python declaration types and referenced variables now belong to their registries.
`PythonNodeDefinition::SignatureDefinition` retains signature and wiring parameter
pointers. `TypeDescriptor` no longer retains semantic-type owners. Python callables,
graph structures, and captured values keep their existing ownership. Registry
storage contains no Python callbacks.

## Type resolution and argument validation

`Signature::resolve(types...)` resolves boundary types. Actual wiring argument
validation checks the boundary types and the parameter's structural constraint
before invoking the graph function. A resolved boundary type alone cannot prove
that a graph value satisfies a structural constraint.

## Verification

Cover stable type identity after graph destruction, different structures sharing
one signature type, exact and pattern overload matching and specificity,
captured-value cache separation, ordered multiple structures, release of
argument and constraint values, and retained diagnostics. Structural rejection
diagnostics retain their reason when expected and supplied types are identical.

On 2026-10-06, the Python-enabled `bg-build` build with `-j12` succeeded.
The full CTest suite passed **1,230 tests with zero failures**: **1,229 C++ tests**
and a Python aggregate containing **118 passing Python tests**. Nodes resolve
scalar-or-time-series alternatives to a selected concrete form. Unions between
time series forms remain graph-only. Python 3.13+ is required; wiring failures
preserve Python exceptions and attach context notes without duplicating tracebacks.

Focused type-system tests passed **197 assertions in 75 cases**, wiring
materialisation tests **355 in 50**, and time-series view tests **65 in 18**.
The node-alternative regressions passed **12 assertions in 3 cases**.
`git diff HEAD --check` passed.

Logs:

- [`build/dev/type_registry_test.log`](../../../wiring-materialisation/build/dev/type_registry_test.log)
- [`build/dev/type_registry_final_ctest.log`](../../../wiring-materialisation/build/dev/type_registry_final_ctest.log)
