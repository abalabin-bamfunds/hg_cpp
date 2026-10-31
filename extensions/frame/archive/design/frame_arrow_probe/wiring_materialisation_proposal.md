# Wiring with materialisation options

Proposal, 2026-10-05. API names are provisional. This document updates the
wiring mechanism proposed in the [frame working note](frame_design_note.md).
It does not describe implemented BG behavior.

## Model

A selected overload produces a wiring point with one semantic output type.
The point can offer several materialisations. Each option declares its target
type, a nonnegative cost, and an owned materialisation callback with declared
inputs and bound arguments. Target types, costs, and input declarations are
available without running callbacks.

A deferred point retains the lexical context at its creation. Its
materialisation callbacks use those bindings during preparation and execution.

Unions between time series forms belong to graph signatures. A node parameter
may declare scalar alternatives plus one time series alternative; wiring selects
a concrete form for that input. A union has no runtime storage. Materialisation
requirements belong to consumer connections; one point can supply several forms
to different consumers.

`Union<"name", A, B, ...>` gives the constraint an explicit identity. Distinct
labels give union constraints independent outer bindings. Variables shared
inside their alternatives still constrain both arguments. Reusing the same
constraint retains BG's existing requirement that its concrete type binding
agree. `Union` aliases the existing named `TypeVariable` declaration.

| Boundary | Effect |
| --- | --- |
| Ordinary graph parameter | Check compatibility and preserve source identity and options without recording a demand. |
| Selected overload, concrete parameter | Record the required form. |
| Selected graph overload, union parameter | Retain compatible alternatives until materialisation. |
| Node input | Record a concrete input requirement. |
| Selected materialisation option, concrete input | Record the required form before running callbacks. |
| Selected materialisation option, union input | Retain compatible alternatives for planning. |

Recording a requirement does not construct the requested output. Overload
selection happens once per call. Graph expansion can introduce further calls;
each follows these rules. Runtime node signatures must be concrete.

A concrete graph parameter receiving an alternate type gets a lazy wiring
point for that selected form. The point exposes the selected type and retains
the source's options, costs, and identity. A Union parameter keeps the original
point. This is required to validate concrete typed arguments without forcing
materialisation at an ordinary graph boundary.

## Phases

1. **Collect typed wiring.** Run graph code, select overloads, establish output
   types, and retain calls, operands, options, scalar arguments, context
   bindings, and diagnostic paths. Include outputless calls and required
   service/lifecycle operations.
2. **Materialise.** Collect known requirements from consumers towards producers.
   Reuse compatible required forms at flexible connections, then use costs
   for remaining choices. Commit each choice before running its materialisation
   callback. Finish binding concrete nodes in producer-to-consumer order and
   cache each selected point/option result.

A committed materialisation choice is final. Expansion can introduce services,
node calls, nested graphs, and further materialisation requirements. Process
that new work while preserving earlier choices and their generated wiring.
Later requirements can influence choices that remain open. Additional consumers
can request other advertised forms of the same point under the existing
per-consumer model. Repeated requests for a completed point/target reuse its
cached result.

Completion continues until all introduced construction work is complete. Only
then does finalisation close wiring. This is required to construct services
and graphs introduced by materialisation callbacks without revisiting the
choices that introduced them.

Expression callbacks can recursively expand expression options and collect
column inputs. Retain source wiring-point identities so the callback can reuse
columns required when it commits its expression computation.
Option expansion can return a lazy bundle; concrete output construction is a
separate operation. Helper calls introduced during expansion contribute their
normal signature requirements before final binding. The second phase can
therefore require recursive expansion rather than one scan.

Low-level callers explicitly call `wiring.finalize()` before constructing a
`CompiledGraph`. `run_graph` and `test_graph` call it automatically. Compilation
remains read-only with respect to graph wiring. Successful finalisation closes
the graph to further wiring. Repeating it is harmless; adding nodes afterward
is an error.
Finalisation failures are reported as wiring errors and stop construction.

Engine code can complete currently collected construction without closing the
graph. Structural graph comparison uses this operation to demand the body
result and finish all body calls before taking a snapshot and adding runtime
boundary adapters. Completion preserves committed choices. Child completion
registers service requirements with the root; root finalisation coordinates
service construction with materialisation and closes only after both finish.

Use `wiring.finalize(catalogue)` to complete a graph with services. The wiring
retains a service build session and its catalogue state. The existing
`catalogue.build(wiring)` entry attaches the same session and constructs known
services while leaving wiring open; later `wiring.finalize()` continues that
session for requirements introduced by callbacks.

Deferred callbacks created inside a multiservice implementation retain access
to its request and reply ports. The service build session owns the port state
until wiring finishes. Duplicate or invalid declarations fail immediately;
missing declarations are checked after deferred construction completes.

For `mid = (q.ask + q.bid) / 2`, a `consume_column(mid)` collected before the
expression consumer materialises requires one concrete `mid`. An expression
consumer accepting that column can reuse it.
Without that requirement, it can expand the arithmetic through expression
options. An expression-only connection still receives its requested expression
form, possibly reading the shared column as a leaf. A service introduced later
that requires a concrete `mid` adds that materialisation while preserving the
earlier expression computation.

### Expression consumers

The expression overload of `filter` runs during collection and returns a
deferred point with one materialisation option. Its output type is established
during collection. The materialisation callback performs expression traversal,
selects required columns for reuse, and constructs the computation using the
requirements collected before it starts.

Early inspection can establish logical structure and output types. It must
retain source point identities and leave representation-dependent expression
processing to the materialisation callback. Forced materialisation requirements
recorded after early inspection remain available to that callback when it starts.

This uses the existing `materialisation_option` API with one option. BG reads
the selected callbacks' input declarations and collects their requirements
before expression preparation. A callback must declare every upstream input
whose materialisation it requires. Concrete input declarations require a form;
union declarations keep compatible alternatives available.

## Matching and costs

- Match against the semantic type and advertised options without running
  callbacks. A deferred point's semantic form uses its declared cost; an
  already concrete output has an identity form at zero cost.
- For a candidate, find type-consistent option combinations and their declared
  costs. Retain viable union alternatives. Union branches have separate type
  bindings; variables exclusive to an unavailable branch need not resolve.
- Shared variables and the semantic output must be determined during
  collection. An expression schema must expose its result type. Differing
  viable output types remain an ambiguity to resolve during collection.
- Proposed ranking: preserve BG's fixed-before-variadic candidate grouping;
  within that group prefer the lowest total option cost, counting an identical
  point/option binding once. Use existing specificity rules at equal cost;
  an unresolved tie is ambiguous. A union accepts a broader set than one of
  its members and is ranked accordingly where constraints are comparable.
- Unconstrained time-series variables bind the semantic type. Requesting an
  alternate structure requires an appropriate constraint, including `Union`.
- Resolvers run when a variable is requested by signature resolution, node
  metadata, a requirement, or another resolver. Complete these requests before
  selecting and caching a candidate. An unused resolver does not run, including
  one used only by an unmatched union alternative. Cached mappings retain values
  and scalar-read dependencies; they do not retain resolver callbacks.
- Costs are stable selection weights. Required-output reuse happens during
  materialisation and preserves selected overloads and committed materialisation
  choices.

## Proposed APIs

### Graph authors

Add `wp<Union<"name", A, B, ...>>` for graph signatures, including graph
overloads. It retains the original point; it does not choose a union branch or
construct a runtime value. Ordinary node declarations continue to resolve
concrete input types.

### Extension authors

Add the following APIs, with typed convenience forms and erased forms taking
BG type objects. These APIs are for graph construction and extensions; node
evaluation code uses ordinary `In<>` and `Out<>` access.

```cpp
auto& wiring = WiringContext::current().wiring();
auto result = wiring.create_wp<RESULT>(lhs, rhs);

// Each callback declares how it accepts the retained arguments.
result.materialisation_option<RESULT>(build_value, 1.0);
result.materialisation_option<EXPRESSION>(build_expression, 0.0);
return result;
```

An expression consumer uses the same API with one option (illustrative typed
callback syntax):

```cpp
// Inside the expression overload of filter.
auto& wiring = WiringContext::current().wiring();
auto result = wiring.create_wp<TSF<SCHEMA>>(frame, predicate);
result.materialisation_option<TSF<SCHEMA>>(
    [](wp<TSF<SCHEMA>> frame, wp<TSB<COLUMN_EXPR>> predicate) {
        // Frame-extension code walks the expression and reuses required columns.
        auto arguments = prepare_expression(predicate);
        return filter_compute(frame, arguments);
    },
    0.0);
return result;
```

`prepare_expression` represents frame-extension expression preparation, not a
new BG API. The callback executes during materialisation.

| API | Contract |
| --- | --- |
| `GraphWiring::create_wp<T>(arguments...)` | Create a graph-owned deferred record with semantic type `T` and retained arguments. |
| `wp::materialisation_option<T>(callback, cost)` | Add one advertised target, declared callback inputs, and an owned callback. Initially allow one option per exact target type. Freeze options at first use. |
| `wp::materialisation_options()` | Inspect advertised targets, costs, and input declarations without executing callbacks. |
| `GraphWiring::expand_materialisation(point, target)` | Return the requested form, possibly as a lazy record. Cache the result and preserve its origin. |
| `GraphWiring::materialisation_required(point, target)` | Inspect concrete requirements recorded so far during planning. |

Expression bundle inspection must preserve original operand points and expose
the operation descriptor as wiring data. Existing free and variadic bundle
records provide relevant behavior; extend their supported access APIs where
needed. An arbitrary runtime TSB does not promise an inspectable expression.

### BG implementation

| Existing component | Required change |
| --- | --- |
| `WiringPoint`, `wp`, `details::WiringRecord` | Store option data and origins on owned records. Expose the extension APIs without requiring direct use of `bg::details`. Keep the point itself a handle. |
| `Signature`, `SignatureResolution`, declaration reflection | Add union parameter matching and per-argument viable forms with their type bindings. Keep the semantic result fixed and final runtime signatures concrete. |
| `WiringArgument`, `WiringArguments` | Retain the original point alongside the forms considered for binding. Expose these read-only to resolvers and requirements; do not masquerade a TSC point as a TSB. |
| `Operator::invoke` | Probe options, rank candidates, and record requirements only for the winner. Include option descriptors/costs in cache keys; store argument binding choices in cache entries. |
| `GraphTemplate`, `WiringTemplate` call handling | Preserve options through ordinary graph calls. Apply input requirements at the selected overload boundary. Supply a real alternate-form record when its structure is inspected. |
| `NodeTemplate::wire` | Record node calls and concrete input demands. Move concrete input binding and final wired-node reuse into finalisation. |
| `GraphWiring` | Own pending calls and consumer requirements. Add `finalize()` to expand selected options, complete requirements, and produce concrete `WiredNode`/`WiredOutput` objects. Preserve existing concrete-output checks in the final binding path. |
| Compilation entry points and nested graph handling | Finalise wiring before concrete compilation and concrete graph-structure comparison. Retain the established context and reference-boundary rules. |
| `WiringDiagnostic` | Report advertised forms, costs, union matches, and ambiguous bindings. |

## Review boundary

Union signatures expose accepted input forms. They do not expose arbitrary
callback bodies. BG collects declared input requirements before executing
callbacks. Requirements discovered through later callback expansion preserve
committed materialisations as described in [Phases](#phases).

BG owns types, overload selection, wiring, materialisation, and execution.
The frame extension supplies expression schemas, operation descriptors,
traversal, batch functions, and computation nodes.

Materialisation callbacks declare their input requirements. Typed callbacks
provide these declarations through their parameters; erased callbacks supply
an explicit signature. This is required to collect upstream materialisation
requirements before running callbacks that prepare expressions. The input
declaration rule is approved.

## Error reporting review

Validate each call as soon as the required declared information is available.
Deferred node construction must preserve call-time checks for argument presence,
argument category, and compatibility of a selected output runtime type with the
resolved output. Retain the selected runtime type for concrete binding.
Calls introduced by a materialisation callback follow the same validation rules
when introduced.

Some failures require callback execution: an incompatible returned point,
dependencies or services first introduced by the callback, and cycles involving
generated wiring. Do not execute callbacks early solely to validate them.

Implemented diagnostic context and propagation are described in
[Wiring errors](../../../wiring-materialisation/docs/graph-wiring.md#wiring-errors)
and [Operator diagnostics](../../../wiring-materialisation/docs/operators.md#diagnostics).
Diagnostic propagation visits only newly discovered context facts, avoiding
dependency-path enumeration and repeated preparation. C++ source locations are
not captured.

Every `Type` referenced by signatures, type maps, caches, or diagnostics must
have process lifetime. Diagnostics retain actual type objects through their
pointers. Dynamic type composition can register unnamed types automatically.
The C++ split is implemented: process-owned generic reference declarations
replace the wrappers owned by `nested()`, and graph arguments own their
structure and payload separately from their stable type. Structural constraints
belong to signature parameters and have the signature's lifetime. They continue
to affect overload applicability and specificity. Operator cache keys retain
argument structures separately from type pointers.
The [type lifetime API proposal](type_lifetime_api_proposal.md) lists the
required changes and declaration migration.

## Implementation status

Work remains uncommitted in `.lanes/wiring-materialisation`, based on
`e52a981b`. On 2026-10-06, the Python-enabled `bg-build` build with `-j12`
succeeded. The full CTest suite passed **1,230 tests with zero failures**:
**1,229 C++ tests** and a Python aggregate containing **118 passing Python tests**.

Focused type-system tests passed **197 assertions in 75 cases**, wiring
materialisation tests **355 in 50**, and time-series view tests **65 in 18**.
The node-alternative regressions passed **12 assertions in 3 cases**.
The latest lane logs are `build/dev/type_registry_test.log` and
`build/dev/type_registry_final_ctest.log`. The main worktree remains clean.

The verified foundation provides:

- Graph-owned deferred points, retained arguments, advertised targets and
  callback input signatures, and cached expansion.
- Signature form matching and completion, named unions, shared type bindings,
  and costs that count each source/target pair once.
- Explicit requirement collection through callback inputs, free collections,
  selected items, and captured or forwarded variadic collections.
- On-demand resolvers for signatures, metadata, and requirements.
- Owned lexical context snapshots with isolated, scope-bound activation.
- Separate expansion and concrete-output caches, compatible lazy callback
  results, and dependency-cycle detection.

Integration now separates node-call identities from concrete node indices and
collects node calls before binding their inputs. It retains all actual calls,
including outputless calls. Output runtime types use main's
`NodeTemplate::output_runtime_type` selection. Compilation requires completed
wiring; execution helpers invoke finalisation. Construction completion, retained
service state, creation-context activation, nested body snapshots, and
option-aware operator invocation and caching are implemented but still under
integration review.

Tests now complete deferred wiring before inspecting concrete structure or
compiling it, keep node templates alive through completion, and use node-call
identities where construction order can change. Lane regressions were corrected
so nested output binding applies the declared schema before materialisation,
switch output inference preserves an existing reference target, and inherited
inputs use their existing input-binding path without redundant reference nodes.

A regression covers materialisation introducing a service whose implementation
adds another materialisation demand. The first result remains cached, each form
is built once, repeated finalisation adds no callback executions, and the graph
compiles. No new design decision was needed for these test repairs.

Diagnostic regressions cover deferred builders introducing services, actual
`nested()` boundaries, shared dependency preparation and consumer origins,
invalid callback results, preservation of operator diagnostics, retained
exception causes and owned contexts after graph destruction, and offered-form
and matched-binding costs without extra callback execution.

Call-time validation now rejects graph arguments passed to nodes, absent
required scalars, and incompatible output runtime selections before retaining
the node call or its demands. Tests verify immediate rejection and successful
completion after rejection. A selector-count test verifies that output runtime
selection runs once at the call and its result is reused during binding.

Every `Type` now registers, including generic and Python declarations, as
described in the [type lifetime API proposal](type_lifetime_api_proposal.md).
Nodes select a concrete scalar-or-time-series parameter form. Unions between
time series forms remain graph-only. Python 3.13+ is required; wiring failures
preserve Python exceptions and attach context notes without duplicating tracebacks.
Final review is complete. Integration remains separate from this uncommitted implementation.
The BG work does not complete the downstream frame extension implementation.
