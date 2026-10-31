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

`get<Scope, Index, Role>(GuaranteeParameter)` receives a `TimeSeriesTypeRole`:
`declaration`, `input`, or `output`. A declaration uses neutral guarantees and
returns a canonical resolved type when its components are concrete. Input
construction retains variables for matching; output construction applies
[output defaults](type-system.md#time-series-guarantees). Propagate the role to
owned time series components. A reference output constructs its target with
the declaration role because it does not own that target.

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

Type factories return the same object for equivalent resolved types and follow
the [semantic type registration contract](type-system.md#type-model). The
provider library must remain loaded while its type objects, runtime types, or
values are in use.

## Runtime types

A provider derives its runtime interface from `TimeSeriesRuntimeType`. The
runtime describes storage, construction, destruction, invalidation, and any
contained time series. Family-specific access and mutation operations belong
to the provider's runtime interface.

A runtime may override `reference_sample_factory()` when dereferencing its
time series requires a sampled view. BG calls the factory on the runtime
requested by the receiving input and passes the requested semantic type, the
previous and current source descriptors, the sampling time, and an allocator.
The returned sample owns one reference and supplies an input view whose runtime
has the requested semantic type. The sample may use the actual runtime types in
the source descriptors to interpret their storage. It may borrow that storage
through the receiving cycle. Return `nullptr` when the transition has no sample.
See [reference sampling](time-series-references.md#input-sampling).

Generic output copying calls the output runtime type's `copy_tick()` or
`copy()` with the input and output bindings. A provider implements these
operations to support [tick and value copying](node-api.md#output-parameters).
The input can use a different runtime representation. Copy through its input
interface and publish through the output binding. The default `copy_tick()`
calls `copy()` for a current-cycle change. Collection providers override it to
copy only changed items. The default `copy()` reports that copying is unsupported.

`time_series_runtime_traits<Declaration>::get(resolved_type)` supplies the
canonical C++ runtime through `cpp_time_series_runtime_type<Declaration>(resolved_type)`.
It must use the supplied resolved type, including inherited guarantees. The
no-argument runtime accessor uses a neutral declaration; direct output bindings
request `TimeSeriesTypeRole::output` to apply output defaults.
The canonical semantic factory records a callback with
`TimeSeriesType::set_default_runtime_type_factory()`, including when it resolves
a generic declaration. The callback takes the resolved semantic type and
returns its trait-defined runtime. Recording the callback does not construct
the runtime. This supports consumers that retain only semantic types, including
empty and skip-only literal inputs.

`register_time_series_runtime_type()` associates one runtime with an exact resolved semantic
type for ordinary runtime resolution. Repeating registration of the same runtime
is allowed. Registering a different runtime for that type is an error. Registered
runtime objects must remain alive while the registry can use them.

The general runtime resolver gives explicitly supplied representations
precedence over registered representations. Output selection follows the
[output declaration](#output-declarations). A node template selects the
output runtime type during wiring. The selected runtime must have the wired
output's resolved semantic type.

Literal sources use default runtime resolution, which calls the semantic
descriptor's factory and recursively obtains component defaults. Supplied
representations do not select literal storage.

Initial storage follows the existing
[byte-copy initialization requirement](type-system.md#runtime-types).
Runtime construction and destruction use the supplied allocator. A provider
reports whether its storage uses variable memory or requires finalization.

## Output declarations

BG declares `Out<Declaration, Options...>`. Built-in time series implement
`Out<Declaration>`. Providers define any additional options and their matching
`Out` specializations.

Include `<bg/node/output_declaration_traits.hpp>` and specialize
`output_declaration_traits<Output>` for a configured output. Its
`time_series_type` identifies the semantic declaration.
`runtime(resolved_type)` selects the representation and must return a runtime
with exactly the supplied resolved semantic type. Selection receives no input
runtimes. These traits are APIs for extension authors; node code uses the
typed output's operations.

The ordinary `Out<Declaration>` trait uses the canonical runtime for concrete
declarations and registered or composed defaults for generic declarations.
Built-in collection children use `Out<ChildDeclaration>` and retain their
actual runtime through the output binding.

Callable, aggregate, and runtime-signature nodes bind the complete output
declaration and use its interface for invalidation. Providers own their option
semantics and any source bindings required by their representations.

Runtime-signature definitions declare `output_declaration_type` for runtime
selection and use `NodeInvocation::output_as<Output>()` for binding.
`output<Declaration>()` binds the ordinary `Out<Declaration>`.

The [node output-runtime hook](node-api.md#output-parameters) replaces trait
selection when the representation depends on wiring arguments. Custom node
templates implement `NodeTemplate::output_runtime_type(const WiringArguments &)`;
BG calls it during wiring and retains its result for compilation.
The [wiring callback](graph-wiring.md#wiring-template-call) receives this borrowed
argument view alongside the normalized arguments used for forwarding.

## Views and publication

The provider supplies `In<Declaration, Properties...>` and `Out<Declaration>`
specializations using the public input/output bindings. Additional output
options use their own `Out<Declaration, Options...>` specialization.
Views borrow storage for one node invocation. Binding must preserve the
selected runtime type.

An input accepts `const TimeSeriesInputData&` and an optional activity-control
pointer. `InputProperties<Properties...>` supplies the declaration's input
metadata. `InputActivityControl` implements activity changes for inputs that
declare dynamic activity. Include `<bg/node/input_properties.hpp>` for these
helpers.

`input_data()` exposes the borrowed input binding to engine and extension
adapters, including generic output copying. It is valid for the input view's
invocation.

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

The [provider test](../../../tests/type/time_series_provider_test.cpp) demonstrates
declarations and views in callable nodes, aggregate nodes, and a built-in
collection.

## Literal updates

Providers register `TimeSeriesLiteralOperations` from
`<bg/time_series/literal_operations.hpp>` with
`TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance()` to support
`Out<TS_T>::apply(const literal::TimeSeriesLiteral&)`. Lookup prefers an exact
runtime registration, then its semantic type.

The handler receives the complete update and the destination's `OutputState`.
It can construct `Out<TS_T>{TimeSeriesOutputBinding{state}}` and call
`as_output<Output>()` to obtain its typed output with the containing output path.
The handler interprets its literal syntax and sentinels, mutates through that
output, and uses its normal publication and invalidation operations. Borrowed
literal sources and output views remain valid for the call.

Providers define additional instructions as instances of
[`literal::Sentinel`](scalar-api.md#sentinels). Their handler compares the
singleton instances and inspects arguments through `literal::TaggedLiteral`.
Scalar and time series handlers receive the same carrier; a provider can pass
scalar payloads directly to scalar construction.

Node authors use `Out::apply()`; registration and `OutputState` are
extension-facing APIs. Scalar literal construction has its own
[operation record](scalar-api.md#literals).

## Queue messages

Providers register `TimeSeriesQueueOperations` from
`<bg/time_series/queue_operations.hpp>` with
`TimeSeriesDispatch<TimeSeriesQueueOperations>::instance()`.
This is an extension-facing API. Graph authors use `Queue::write()`.

Register `project`, `write`, `conflicts`, and `apply` for each supported
exact output runtime. Register `discard` for its semantic time series type.
Semantic registration does not enable queue writes to other runtime layouts.
BG validates these registrations when it selects the queue output runtime.
Batch queues reject provider-defined time series leaves.

BG owns message framing, scheduling, invalidation, and traversal through
built-in collections. Provider callbacks encode and process only their valid
leaf payload:

- `project(literal, projection)` validates the literal and advances the aligned
  payload extent before reservation. It also runs during writing.
- `write(literal, writer, allocator)` constructs an owning payload with the
  projected layout. Each completed prefix must be discardable if writing fails.
- `conflicts(item, now, incoming_valid, reader)` inspects borrowed output storage
  and encoded values without mutation or destruction. Missing collection items
  have null storage. An invalidation has no payload. Returning `false` for a
  valid payload must advance past that complete payload. Returning `true` may
  stop early; BG leaves the complete message pending.
- `apply(reader, output)` consumes an accepted payload and publishes through
  the output binding. It must consume and destroy or transfer the complete
  payload even when it throws. BG discards later siblings after a failure.
- `discard(reader)` advances past one payload and destroys its owning objects.
  During failed-write cleanup, destroy completed objects before an incomplete
  trailing field reports `std::out_of_range`.

Standard mode calls `conflicts` before applying a message. Eliding mode applies
messages in order without conflict inspection. Provider registrations must
finish before concurrent producers start. The provider library must remain
loaded while callbacks or encoded payloads remain in use.

Queue payloads and retained tick records have separate registrations and may
use different grammars. See [queue messages](time-series-types.md#queue-messages)
for framing, admission, and failure behavior.

## Generic visitation

Generic input/output visitors dispatch built-in structural families to their
matching views. An external family reaches the erased `In<TS_T>` or
`Out<TS_T>` callback. The erased output retains its actual runtime type and
output binding. The callback decides how its operation handles that family.
It must not recursively redispatch the same
erased view without selecting an implementation.

A provider's typed `In` and `Out` views forward `visit()` to the corresponding
erased view. This lets generic operations inspect a provider view and dispatch
back to its typed handler through the actual runtime type.
An erased input view's `as<Declaration>()` checks semantic compatibility,
including compatible generic declarations, before it constructs a provider
view; the returned view borrows the same invocation data.
For a configured output, `as_output<Output>()` checks semantic compatibility
and constructs the complete output declaration over that binding.

The visitor's view is valid only during the callback. Optional operation
handlers may retain a runtime-type reference, whose lifetime is independent
of the invocation.
