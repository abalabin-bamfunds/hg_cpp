# Time series providers

## Purpose

A provider adds a time series family from a separate library. It supplies the
semantic type, runtime representation, and typed input/output access. BG binds
those views to graph storage and performs publication through its output
binding.

The [scalar provider contract](scalar-api.md#scalar-providers) defines scalar
values exposed by a time series provider. The two providers can belong to the
same library.

## Declarations

A provider specializes `time_series_type_traits<Declaration>` to describe the
semantic type and whether the declaration is supported or generic.
`TimeSeriesDeclaration<Declaration, Properties...>` supplies `type()` and
guarantee metadata for declarations with BG guarantee properties. Include
`<bg/time_series/time_series_type_traits.hpp>` for the customization point.

`declaration_variable_traits<Declaration>::for_each` enumerates variables in
the declaration. It must include component variables and guarantee variables.
`for_each_declaration_variable` recurses into a component declaration through
the same public customization point. Include `<bg/type/declaration_variables.hpp>`
for these interfaces. `TimeSeriesDeclarationProperties<Properties...>::for_each_variable`
enumerates guarantee variables using BG's canonical variable objects.

The semantic type implements matching and resolution through `Type`. A
resolved time series type implements `with_guarantee()` using canonical types.
This is required to preserve the [guarantee rules](type-system.md#time-series-guarantees)
when declarations are matched or combined.

Type factories return the same object for equivalent resolved types. The
provider library must remain loaded while its type objects, runtime types, or
values are in use.

## Runtime types

A provider derives its runtime interface from `TimeSeriesRuntimeType`. The
runtime describes storage, construction, destruction, invalidation, and any
contained time series. Family-specific access and mutation operations belong
to the provider's runtime interface.

`time_series_runtime_traits<Declaration>::get(resolved_type)` supplies the
canonical C++ runtime through `cpp_time_series_runtime_type<Declaration>()`.
It must use the supplied resolved type, including inherited guarantees.
The provider registers defaults with `register_time_series_runtime_type()`
when it creates resolved family types, including resolution of generic
declarations. This makes the default available before graph compilation
requests it.

Registration associates one default runtime with an exact resolved semantic
type. Repeating registration of the same runtime is allowed. Registering a
different default for that type is an error. Registered runtime objects must
remain alive while the registry can use them.

The general runtime resolver gives explicitly supplied representations
precedence over registered defaults. Output selection follows the
[output specification](#output-specifications). Extension implementations can
override that selection through `GraphWiring::set_output_runtime_type()`.
The selected runtime must have the wired output's resolved semantic type.

Initial storage follows the existing
[byte-copy initialization requirement](type-system.md#runtime-types).
Runtime construction and destruction use the supplied allocator. A provider
reports whether its storage uses variable memory or requires finalization.

## Output specifications

`Out<Declaration>` uses `OutputSpecification::default_`.
`Out<Declaration, view>` uses `OutputSpecification::view`. The specification
belongs to the output declaration; both forms expose the same semantic time
series type to consumers.

Include `<bg/time_series/cpp_runtime_type.hpp>` and specialize
`time_series_output_traits<Declaration, Specification>::runtime(resolved_type)`
to select a representation. The result must have exactly the supplied resolved
semantic type. Selection receives no input runtimes. A view representation
retains source runtime information in its source bindings.

The default trait uses the declaration's canonical runtime for concrete
declarations and resolves registered or composed defaults for generic
declarations. This also selects default child representations in collections.
There is no default implementation for the view specification. A provider
supporting it supplies both the runtime trait and the corresponding
`Out<Declaration, view>` specialization.

Callable and aggregate node definitions preserve the complete `Out`
declaration for runtime selection, invocation binding, and invalidation.
Explicit `GraphWiring::set_output_runtime_type()` bindings take precedence;
their selected storage must support the declared output operations.

## Views and publication

The provider supplies `In<Declaration, Properties...>` and `Out<Declaration>`
specializations using the public input/output bindings. Additional output
specifications use their own `Out<Declaration, Specification>` specialization.
Views borrow storage for one node invocation. Binding must preserve the
selected runtime type.

An input accepts `const TimeSeriesInputData&` and an optional activity-control
pointer. `InputProperties<Properties...>` supplies the declaration's input
metadata. `InputActivityControl` implements activity changes for inputs that
declare dynamic activity. Include `<bg/node/input_properties.hpp>` for these
helpers.

An output accepts a move-only `TimeSeriesOutputBinding`. The binding owns or
borrows an `OutputState` and releases owned invocation storage when destroyed.
The provider retains that binding as a member of its output view. A binding
constructed from an existing `OutputState&` borrows the state; that state must
outlive the binding.

An input exposes BG's [common input operations](node-api.md#complete-time-series-access)
alongside its family-specific reads. Its binding refers to `TimeSeriesState`,
which preserves the latest change time and validity transition. An output
uses BG's publication and invalidation operations after updating its own
storage and change information.
The output binding carries the containing-output path for collection children.

Include `<bg/time_series/input_data.hpp>`,
`<bg/time_series/output_binding.hpp>`, and
`<bg/time_series/output_state.hpp>` for these interfaces.
These bindings are APIs for extension authors. Node code uses the typed
`In` and `Out` operations supplied by its provider.

Providers use the existing evaluation context for allocation and cycle
callbacks. A callback may retain runtime storage with the required lifetime;
it must not retain an invocation-scoped input or output view.

The [provider test](../tests/type/time_series_provider_test.cpp) demonstrates
declarations and views in callable nodes, aggregate nodes, and a built-in
collection.

## Generic visitation

Generic input/output visitors dispatch built-in structural families to their
matching views. An external family reaches the erased `In<TS_T>` or
`Out<TS_T, Specification>` callback. Output visitation and collection item
access preserve the output specification. The callback decides how its
operation handles that family. It must not recursively redispatch the same
erased view without selecting an implementation.

The visitor's view is valid only during the callback. Optional operation
handlers may retain a runtime-type reference, whose lifetime is independent
of the invocation.
