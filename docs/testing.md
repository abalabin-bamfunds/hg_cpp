# Testing

## Purpose

The test suite verifies graph correctness, supported interfaces, and performance
constraints. Node and graph behavior tests construct graph code through public
node and graph APIs and use `test_graph`. Tests for engine invariants use the
engine API that owns the invariant.

## Coverage model

Structured Catch2 tags are the source of coverage information. Generated
coverage reports read these tags. Do not maintain a separate list of covered
combinations.

Use `[dimension:value]` for each applicable dimension:

- `feature`: `compilation`, `compilation-order`, `inherited-time-series`, `scalar-api`, `scalar-storage`,
  `scalar-collection`, `global-state`, `visitor`, `type-system`,
  `node-api`, `state`, `wiring`, `context-time-series`, `graph-compilation`, `test-graph`, `evaluation-context`,
  `evaluation-scratch`, `borrowed-input`,
  `lifecycle`, `finalization`, `scheduling`, `timed-scheduling`, `simulation`, `realtime`, `wall-clock`, `profiling`,
  `input-activity`, `validity`,
  `invalidation`, `operator`, `constant`, `graph-argument`, `map`, `switch`,
  `time-series-storage`, `time-series-view`, `collection-change`, `reference`, `sampling`, `feedback`,
  `item-id-remapping`, `service`, `getitem`, `getattr`, `nested-graph`, `mesh`,
  `multiservice`, `reduce`, `late-bound-wiring`,
  `variadic-wiring`, or `wiring-materialisation`
- `series`: `value`, `fixed-tsl`, `tss`, `tsd`, `tsb`, `reference`, or `generic`
- `schema`: `named`, `unnamed`, or `generic`
- `representation`: `native`, `free`, or `covariant`
- `scalar`: `byte`, `i64`, `integer`, `floating-point`, `boolean`, `string`, `symbol`, `evaluation-time`, `timedelta`,
  `opaque`, `compound`, `hierarchical`, `list`, `tuple`, `set`, `dict`, or `generic`
- `storage`: `inline`, `fixed`, or `variable`
- `declaration`: `concrete`, `partially-generic`, `generic`, or `runtime`
- `binding`: `direct`, `value-to-reference`, `reference-to-value`, `reference-to-reference`, `covariant`, or
  `representation`
- `shape`: `single-node`, `chain`, `fan-out`, `diamond`, `nested`, `doubly-nested`, `deeply-nested`, `multiple`,
  `source`, or `sink`
- `activity`: `active` or `passive`
- `validity`: `required`, `optional`, or `all-valid`
- `guarantee`: `ordinary`, `deduplicated`, `final`, or `const`
- `input-guarantee`: `valid-items`
- `transition`: `first-tick`, `update`, `invalidation`, `revalidation`, `addition`, `removal`, `resurrection`,
  `start`, `stop`, `restart`, or `lifecycle`
- `definition`: `aggregate`, `function`, or `runtime`

Use lower-case kebab-case values in tags. For example:

```cpp
TEST_CASE("a reference input samples its target",
          "[feature:reference][series:value][binding:reference-to-value][shape:single-node]")
```

A test can omit a dimension that cannot affect the behavior at its test level.
If values from two tagged dimensions do not form a complete combination, split
the test. Mark utility tests that do not participate in graph coverage with
`[coverage-exempt]` instead of structured coverage tags.

Add each feature and its applicable dimensions to the test areas below before
implementing it. Select combinations that exercise each code path,
compatibility boundary, failure mode, and regression. Use pairwise combinations
for independent dimensions and explicit combinations for higher-order behavior.

With Python 3.13 or newer, build the `test-coverage-report` target to validate
the tags and write `test-coverage.md` to the build directory.

## Test layout

- `tests/utils`: independent utilities
- `tests/value`: scalar and time series value access and storage
- `tests/type`: type matching and resolution
- `tests/wiring`: graph construction and wiring-template calls
- `tests/graph`: graph compilation and compiled layout
- `tests/evaluation`: evaluation context, engines, lifecycle, scheduling, storage, time series, and validity
- `tests/node`: node declaration and invocation interfaces
- `tests/testing`: test utilities
- `tests/library`: standard library overloads, in the separate `bg_library_tests` binary

Name a test file after the feature it covers. Split node and graph behavior by
value type when the type changes the expected behavior.

Start each file with a simple test of its core behavior. Order the remaining
tests from simple cases to specialized behavior and failures. Place declarations
needed only by later tests after the first test.

Keep each test case focused on one behavior. Define a test-only function, node,
or graph in the test case that uses it. When an interface requires a
namespace-scope type, place its declaration immediately before the test case.
Keep other declarations at file scope only when multiple test cases share them.
Separate node and graph definitions with a blank line.

Tests may use `using namespace bg;` to avoid repeated qualification.

Use the simplest applicable [node declaration](node-api.md). Prefer plain C++
parameters and a return value for scalar computations:

```cpp
const auto increment = node([](i64 value) { return value + 1; });
```

Use `In<>` for time series operations or constraints that plain parameters
cannot express, and `Out<>` for explicit output modification. Use an aggregate
definition when required by the behavior or declaration form under test.

Keep the graph, input sequence, and expected results visible in the test case.
Prefer literal input sequences and infer their types from the graph signature.
Specify an input type only when the signature leaves it unresolved or selecting
a runtime representation is part of the test. Keep a custom source when its
declaration, lifecycle, or output mutation is under test.

Assert output deltas when they establish the behavior. Use test-local counters
or event logs for evaluation counts, callbacks, and lifetime requirements that
outputs cannot establish. Observe input properties through normal node APIs.

Before replacing a test, identify the requirement established by each assertion
and preserve it in the replacement. Add cases for concrete gaps found during
this review. Coverage tags identify tested combinations; assertions must
establish the claimed behavior. A test that claims independent behavior across
items or graph instances must exercise and observe more than one.

`test_graph` accepts a wiring template or a graph function. Each input range
defines updates by outer cycle. An empty `std::optional` skips its cycle. The
result has one position per cycle and uses an empty position when the output did
not update.

Pass shared [graph run options](graph-wiring.md#direct-execution) immediately
after the graph to configure [graph services](services.md) and profiling:

```cpp
auto options = GraphRunOptions{
    .services = &catalogue,
    .recorder = profiler.recorder(),
};
auto actual = test_graph(graph, options, {1, 2}, {10, 20});
```

Omit the options argument to use the defaults:

```cpp
auto actual = test_graph(graph, {1, 2}, {10, 20});
```

`test_graph` requires `EvaluationEngine::simulation` and rejects realtime
selection. `GraphRunOptions::start_time` sets the first input time and the origin
of captured result positions. Input positions remain one microsecond apart.
Defaults and shared options follow [direct execution](graph-wiring.md#direct-execution).

Literal input sequences use the [output literal syntax](node-api.md#output-parameters).
Each sequence element describes one cycle's delta. Source nodes borrow the
sequences for the synchronous test run and call `Out::apply()` for each tick.
Use `literal::SKIP` for a skipped cycle:

```cpp
auto actual = test_graph(dictionary_sum, {
    {{"a", 10}, {"b", 20}},
    {{"b", literal::REMOVE}},
    literal::SKIP,
    {{"a", 11}},
});
CHECK(actual.equals<i64>({30, 10, std::nullopt, 11}));
```

`{}` supplies no ticks; `{{}}` supplies one empty collection tick.

Literal inputs require a non-variadic parameter list. Concrete time series parameters and plain node
parameters supply input types. A plain node parameter supplies its time series alternative; its value
type must be resolved. Input types come from the signature, so empty and skip-only sequences work too. Use
`input<TSD<string, TS<i64>>>({...})` for an unresolved input declaration, or
`input(runtime_type, {...})` to supply a time series runtime type explicitly.
Graph functions with non-collection scalar inputs preserve their existing
brace-input C++ element conversions and `std::nullopt` support.
Raw brace calls support one through four inputs. Pass `TestGraphLiteralInput`
descriptors or a contiguous range of descriptors for more inputs. Construct
span-backed sequences with `TestGraphLiteralInput(ticks)`. Construct
descriptors from temporary values in the `test_graph` call so their sources and
nested initializer lists remain alive throughout evaluation.

Result capture supports `TS`, `TSL`, `TSS`, `TSD`, and `TSB`, including nested
collections. A result owns the serialized deltas in a shared buffer chain.
Built-in comparisons read borrowed payloads. Indexed scalar access returns an
owning `ScalarValue<T>` copy in `std::optional`; call `.view()` to use the
scalar interface. The copied value can outlive the result.

`visit_tick(index, visitor)` reads a captured tick without copying its scalar
values. It calls `visitor(type, valid, reader)` after the root header and returns
`false` for a skipped cycle. An invalid tick has no payload. The visitor borrows
the reader and scalar values only for its call; it must leave stored values
intact and read at most one payload. Repeated calls read the same tick.

Top-level reference outputs use ordinary reference-to-value binding. Capture
rejects reference-valued collection items. Provider-defined time series types
use their registered `TimeSeriesTickOperations` write and discard handlers.
Register `TickLiteralComparison` by semantic time-series type to compare their
literal payloads. The framework handles invalid ticks before calling the
comparison handler. A handler reads a valid payload from the supplied reader,
advances past exactly one payload on success, and leaves encoded values intact.

For comparisons across cycles, set `TickLiteralComparison::create_state` instead
of `compare`. Each `equals()` call creates a separate `TickLiteralComparisonState`
for each nested time series lifetime. Its `compare()` receives the zero-based
output cycle index, including skipped cycles. BG checks invalidation expectations
and calls `invalidate()`, including when an enclosing collection invalidates.
`finish()` checks the accumulated result when the time series is removed or the
comparison ends. Removing and re-adding a collection item creates new comparison
state. Setting `create_state` takes precedence over the stateless callback.

Scalar expectations use [scalar literal construction](scalar-api.md#literals),
including provider handlers. Collection comparisons interpret removal
instructions at their defined positions. Other bare and tagged
[sentinels](scalar-api.md#sentinels) reach the scalar consumer or extension
comparison handler unchanged.

Compare collection output deltas with the same literal syntax used for inputs:

```cpp
auto identity = [](wp<TSD<string, TS<i64>>> input) { return input; };
auto actual = test_graph(identity, {
    {{"a", 10}, {"b", 20}},
    {{"b", literal::REMOVE}},
    literal::SKIP,
});
CHECK(actual.equals({
    {{"a", 10}, {"b", 20}},
    {{"b", literal::REMOVE}},
    literal::SKIP,
}));
```

Each expected tick describes the complete delta for that cycle. Set item order
and collection entry order do not affect comparison. Use `literal::SKIP` for no
publication, `{}` for a published empty collection delta, and `literal::INVALID`
for invalidation. Scalar `equals<T>()` and indexed scalar access retain their
existing `std::nullopt` behavior for skipped and invalidated cycles.

`test_wiring` creates a `GraphWiring` and `WiringContext` for a `void` callable.
Use it when the behavior under test ends during wiring and does not require
graph compilation or evaluation.

## Test areas

### Utilities

- compile-time strings, symbols, named arguments, aggregate reflection, and C++ schema reflection and construction
- exact-type schema field offsets, const field references, shadowed fields, empty bases, multilevel inheritance,
  aligned and tail-padded layouts, concurrent first use, and construction without intermediate base moves
- buffer allocation, contiguous chunks, chain rollover, concurrent reservations, ordered commit visibility,
  ring-full rollover, allocation-failure rollback, and reservation-scoped reads of completed prefixes for cleanup
- constant-size scalar value handles
- visitor dispatch for concrete and generic scalar and time series views, including base-interface fallback, no
  matching overload, external scalar and time series dispatch, runtime-type precedence, and recursion through collections
- thread-local context storage
- scope-bound cleanup and release
- scheduling bit iteration
- stable-address storage, shared occupancy, and incremental hash containers

### Value system

- canonical scalar conversion
- borrowed and owning scalar lifetime, including owning copies retained across
  successful and failed time series scalar replacement
- consuming scalar transfer, compatible allocator resources, representation
  conversion, and assignment failure cleanup
- typed and erased scalar access, casts, equality, and hashing
- fixed, variable, inline, and graph-accounted storage
- construction, assignment, copy, and destruction
- opaque values and scalar collections, including generic and nested types
- fixed-length scalar tuples: positional type identity and compatibility, generic
  resolution, typed and erased item access, conditional equality and hashing,
  literal construction, shared ownership, and partial-construction cleanup
- compound scalar metadata, native access, generic field access, nesting, equality, hashing, and invalid declarations
- hierarchical scalar identity, schema metadata, inheritance, field narrowing, and container variance
- schema-capturing hierarchical scalar views and construction, runtime field inspection through base annotations,
  shared-handle ownership, and programmatic field-count, type, and presence validation
- provider-defined canonical and copy runtimes, borrowed read-view descriptors, generic source-runtime preservation,
  and publication through time-series outputs
- erased owning-copy runtime selection, typed selector priority, independent snapshot storage, destination
  size and alignment, and construction-failure cleanup
- scalar buffer provider writes through reserved contexts, byte and typed field alignment, and owning value replay and
  discard lifetime; value-dependent provider projection, write-handler selection, and missing projection rejection
- provider scalar literal construction through typed and erased values, runtime registration precedence,
  nested construction, unsupported destinations, and construction-failure cleanup
- shared literal sentinel constants and scalar types across libraries, scalar-wrapper recognition,
  and borrowed argument identity
- time series validity, update time, mutation, and invalidation
- global state ownership, replacement, lookup, and allocation

### Type system

- resolved type identity and scalar or time series classification
- process-owned dynamic declarations and type-pointer validity after graph or wiring-template destruction
- type registry lookup, schema metadata construction, and scalar string reads
  from process shutdown callbacks
- canonical factory cache identity, nested construction, failed factory retries, and concurrent cold misses
- formal type-name formatting through `Type`, including nested extension types
- registration of primitive, generic, extension, and graph types; allocation-free primitive registration;
  independent variable identities and failed-registration rollback
- type variables, constraints, repeated variables, and lower bounds
- type-map overlays and functional variable resolution
- signature matching, resolution, and parameter category inference
- scalar-or-time-series node parameter resolution to a selected concrete form;
  graph-only unions between time series forms
- plain callable and aggregate parameters mixed with explicit views, optional
  values, canonical conversion, reference and constant bindings, scalar return
  publication, and scalar-only scheduling
- hierarchical scalar inheritance and directional compatibility through scalar containers
- shared schema identity across translation units and libraries, C++ and runtime declarations,
  ordered fields, inheritance, optional presence, field-variable resolution, and recursive registration rollback
- schema variables shared across reflection adapters, repeated-variable matching, and owning type field validation
- hierarchical scalar schema capture through scalar arguments, time series values, and scalar collections;
  canonical type identity shared by C++ declarations and programmatic schema construction
- operator signature specificity and ambiguity
- types passed as scalars
- guarantee compatibility, propagation, and enforcement; C++ variable minima,
  preservation of stronger guarantees, repeated-variable resolution, and overload ordering
- recursive common types, independent root and item guarantees, reference targets,
  schema identity and inheritance, and rejection of incompatible structures
- callable scalar-return guarantee inference across scalars, time series,
  references, passive inputs, optional values, explicit outputs, startup
  scheduling, equality support, and nested graph boundaries
- neutral type declarations, input matching, and output defaults through
  collections and reference targets, for callable and aggregate definitions
- reference guarantee limits through unconstrained time series variables, collections,
  and unions; explicit reference targets; rollback of rejected structural matches
- collection types and nested types
- external declarations, public variable enumeration, runtime registration, and selection of supplied representations
- extension-owned configured output declarations, runtime selection, semantic compatibility, and preservation through invocation and invalidation
- definition output-runtime hooks for aggregate, callable, and runtime-signature nodes; resolved scalar configuration,
  flattened variadic arguments, operator cache hits, early semantic mismatch, default trait selection, node-call
  deduplication, and descriptor preservation through compilation and evaluation
- typed and runtime queue-node declarations, scalar-only parameters, injected handle exclusion,
  lifecycle callback compatibility, output runtime selection, mode validation, and per-node mode metadata
- default output representations independent of input representations, including collection children

### Wiring

- wiring-context nesting and thread isolation
- compile-time and runtime wiring-template invocation
- scalar and time series argument resolution
- optional scalar arguments and scalar ownership
- node-template and graph-template interchangeability
- explicit positional type arguments, named type mappings, and mapping conflicts
- graph-template boundaries and inner graph isolation
- wiring paths, labels, and diagnostics
- node-call deduplication, opt-out, and identity inputs
- operator overload applicability, selection, on-demand metadata and requirement resolvers, error handling, and caching
- declarative overload registration through explicit construction, including
  callable argument forwarding, aggregate definitions, class-template families,
  existing node and graph templates, registration order, retained callable ownership,
  reflected template names in wired nodes and diagnostics, and naming isolation
  between template copies and shared aggregate metadata
- unsupported output guarantees rejecting candidates without blocking applicable
  overloads in either registration order; resolver callback exceptions remaining fatal
- graph requirements, including resolved scalar inspection, rejection before body wiring, overload fallback, and diagnostics
- `opt` argument matching, required-input preference, cache separation, named arguments, and variadic forwarding
- required scalar preference over absence-accepting parameters, including
  promoted forms, absent arguments, type specificity, conflicting preferences,
  and mixed scalar and time series calls
- context time series declaration, lexical binding, snapshot capture and activation, explicit arguments, type
  resolution, diagnostics, and caching
- graph arguments, including signature type resolution, repeated variables, exact and pattern matching,
  structure-based caching, and callable and aggregate graphs
- graph signature types shared across different structures, constraint specificity, captured-value cache separation,
  and release of argument and overload constraint values
- incompatible types, graph ownership errors, and invalid results
- free TSL and TSB construction from vectors and spans, inferred types and
  guarantees, explicit empty TSL item types, named fields, and argument-storage lifetime
- variadic capture by TSB and homogeneous TSL, forwarding, materialization, exact-name direct collection binding,
  nested boundaries, and operator selection
- signature wrappers for positional and named variadic graphs and nodes, type mapping, and output validation
- deferred wiring points, retained arguments, advertised materialisation options, signature form matching and completion,
  deduplicated materialisation cost, callback non-execution during matching and root collection, shared concrete demands
  across Union and concrete consumers, retained context bindings, failed transitive collection, unforced expansion,
  lazy results, and expansion caching
- immediate validation before pending-call registration; deferred errors retaining consumer, parameter, collection-item,
  and service contexts; shared request origins; callback exception causes; diagnostic lifetime; offered-form and
  matched-binding costs without additional callback execution
- map argument selection, graph-function adaptation, overload selection, graph identity, and diagnostics
- mesh argument selection, dependency selection, output inference, overload selection, and diagnostics
- reducer graph adaptation, input and output compatibility, TSD item type resolution, and diagnostics
- switch case preparation, argument selection, overload selection, compatibility, and diagnostics
- service declarations, signature validation, type mapping, scalar multiplicity, installation arguments,
  implementation selection, multiservice claims and request/reply ports, and path-aware diagnostics

### Graph compilation

- topological order and cycle rejection
- node records, offsets, alignment, and size limits
- packed scalar, state, input, output, and view data
- runtime storage initialization by byte copy
- subscriber data for activity and validity changes
- start, stop, invalidation, and timed scheduling metadata
- repeated compilation and independent runtime graphs
- scalar and state destruction
- callable capture destruction after compiled graph release and compilation
  failure; typed callable metadata omission with dynamic activity and provider
  runtime access
- const nodes and their direct bindings
- `default_` adapter-node lowering, reference materialization, nested graph returns, and fallback identity
- service binding metadata alongside inherited time series metadata
- borrowed input slot metadata and bindings

### Evaluation

- `test_graph` input sequencing through brace literals, typed ranges, and runtime-selected types;
  scalar construction, nested collection deltas, removal, skip, invalidation, and revalidation;
  plain callable and aggregate parameter inference, explicit generic input types, cycle positions,
  configured start times, realtime rejection,
  services, profiling, and existing scalar input compatibility
- `run_graph` default and explicit simulation selection, realtime selection, engine-specific default start times,
  explicit start times, realtime stop-token forwarding, and services, global state, and profiling in both modes
- `test_graph` output capture and literal comparison for scalar and nested collection deltas;
  scalar ownership after graph destruction, complete delta comparison, entry order independence,
  empty ticks, skipped cycles, invalidation, revalidation, and removal;
  provider scalar expectations, extension comparison handlers, and sentinel identity and argument preservation
- start, evaluation, and stop order
- rejection of user lifecycle functions with time-series-resolved plain fields;
  invalidation placeholders, revalidation, and automatic state and dynamic-input cleanup
- one node evaluation per outer cycle
- scheduling during evaluation, including nodes in the current bit block
- active and passive inputs, including per-input runtime activity changes
- required-input validity caching, invalidation, and revalidation
- immediate `all_valid` queries and evaluation gating across native, free, and reference-bound collections;
  deferred item checks for passive and dynamically inactive inputs, including timer- and other-input-driven
  evaluation attempts; optional bundle fields and base-schema narrowing;
  invalid, removed, and retained dictionary items; constant and default bindings;
  and typed, generic, and runtime node declarations
- operation-specific handling of invalid `opt` arguments, including mixed required and optional inputs
- output update and invalidation propagation
- borrowed inputs supplied per runtime graph invocation, runtime-type access, update/invalidation/revalidation,
  and isolation between nested instances of the same compiled graph
- tick and complete-value copying through typed and generic outputs, current-cycle filtering,
  collection reconciliation, empty first ticks, validity transitions, free collection inputs,
  nested changes, reference values, input guarantees, output guarantees, and provider runtime dispatch
- retained tick writing and replay through a one-cycle delay node, current-cycle filtering, delta-only collection
  replay from a matching baseline, nested key changes, empty first ticks, validity transitions, payload cleanup,
  provider scalar handlers, and unsupported references
- queue message serialization for scalar and collection literals, frame targets and sizes, root skip, malformed
  selectors and markers, scalar ownership, concurrent reservation rollover, and failure cleanup
- queue handle copying and moving, independent concurrent writers, closed-write rejection before serialization,
  retained handles after controller destruction, and shutdown waiting for accepted writes, including failures
- single-consumer engine wakeups, concurrent notification coalescing, publication visibility after wake,
  timed waits, failed-write notification, and wakeup lifetime through queue closure
- outer runtime graph queue ownership, shared admission across target nodes, closure before stop callbacks,
  typed unread-message cleanup after stop, startup and evaluation failure, finalization without stop,
  retained handles after graph destruction, inner graph rejection, and optional runtime storage
- queue-node ordinary output requirements, start and stop callbacks, scalar conversion and optional values,
  callback captures, destination selection, independent runtime graphs, retained writer copies,
  and absence of initial evaluation
- outer-cycle queue draining in standard and eliding modes, publication once per changed target, validity
  transitions, no-op advancement, whole-message pending, cross-target ordering, collection-item independence,
  cleanup before application, and fail-stop cleanup after application errors
- queue scheduling from committed messages, timer priority at cycle entry, minimum-tick chains through inner
  graphs, timer cancellation, queue-only simulation progress, and stopped-graph readiness
- committed queue readiness independent of timer deadlines and wakeup attachment before graph startup
- batch queue output validation, element literal encoding, ordered accumulation per time series, batch replacement
  across cycles, nested collection items, whole-message pending at invalidation and removal boundaries,
  owning scalar replay, unread-payload cleanup, and failure cleanup
- queue message reading for sentinel skipping, target order, padding, block rollover, committed reservation order,
  standard retry, incompatible batch output rejection, no-op advancement, typed discard, replay failure,
  and concurrent watermark growth
- queue tick application in standard and eliding modes, independent collection items, complete-message pending,
  first-conflict stopping, no-op timestamps, invalidation, revalidation, per-key dictionary tick/removal separation,
  encoded scalar ownership during inspection and replay, consuming ownership transfer, and unread-payload cleanup
  after application failure
- feedback source and sink wiring, next-cycle replay, first ticks, validity changes, guarantee compatibility,
  separate handles, and same-graph binding
- `default_` fallback selection, timestamps, modified state, and source validity transitions
- invalidation behavior selected by a node
- timed scheduling, cancellation, ordering, coalescing, stale handles after slot reuse,
  owner-node isolation across nested graphs, and graph-stop cleanup
- timer registration allocation rollback, startup and evaluation failure cleanup,
  finalization without stop, and timers registered by shutdown callbacks
- wall-clock timer readiness independent of evaluation time, overdue coalescing,
  mixed timer progression in simulation, and timer priority over queue input
- fired-handle queries throughout an outer cycle, including nested pause/resume and
  cancellation after activation; reset at the next cycle and graph stop
- simulation time progression and termination
- realtime cycle selection at exact timer boundaries, distinct exact deadlines, queue wakeups during timer waits,
  wall-clock timer readiness and coalescing, and pending queue priority through minimum-tick chains
- realtime idle and timed waits, cancellation before startup and between complete cycles, future-start waiting,
  queue closure on normal and failed runs, retained handles, and profiling
- periodic callbacks at complete outer-cycle boundaries, steady-clock intervals independent of simulation time,
  idle and future-start waits without extra cycles, and exception cleanup with and without profiling
- system and injected wall-clock access during lifecycle, nested evaluation, invalidation, and failure cleanup
- shared global evaluation context and outer graph/evaluator identity across nested lifecycle calls;
  isolation between runs and invocation-local bindings, graph inputs, and scratch frames
- profile records for completed outer cycles, nested graph instances, graph lifetime, polling visibility,
  and rollback after evaluation failure; plain evaluation without a profile
- profiler node and outer-cycle histories with independent periods and retention, steady-clock durations,
  post-run enumeration and reporting, graph-ID reuse, and final-period closure
- outer and inner graph failure behavior
- cycle callbacks and deferred storage reclamation
- global state shared by outer and inner graphs
- context time series startup sampling, changes, validity, and use across nested graph boundaries
- mapped TSD unions, selected key sets, per-key inner graph evaluation, scheduling, and lifecycle
- inner graph input reference guarantees, generic and explicitly typed map
  functions, reference-bound collections, and mapped item replacement while
  another input keeps the inner graph alive
- mesh dependency creation, key changes, shared dependencies, cycle rejection, evaluation order, and lifecycle
- reduction through compact and nested reducers, odd item counts, defaults, sparse updates, physical item-ID changes,
  and membership changes
- switch selection, default selection, inner graph evaluation, scheduling, lifecycle, and composition with map and mesh
- service request deduplication, invalidation, nested graph binding propagation, provider ordering, and client removal

### Time series references

- direct reference access and sampling
- value-to-reference and reference-to-value bindings
- reference changes and value observers
- dynamic subscribers and static subscriber entries
- references across inner graph boundaries
- dynamic collection items and outer-cycle removal lifetime
- representation creation and reuse
- dictionary item selection with scalar and time series keys
- fixed-list item selection with scalar and time series indices
- bundle field selection through value and reference fields, including nested graph boundaries
- keyed subscriptions for present, missing, removed, and restored items

### Collection time series

- set additions and removals
- sampled set and dictionary item-ID remapping across reference changes, including ID exchanges and ordinary deltas
- fixed-list updates
- bundle fields and inheritance
- dictionary key, item, and validity changes
- nested dictionary representations and shared key sets
- output mutation paths and parent notifications
- literal application through typed and erased outputs, mixed nested collections,
  selected updates, empty first ticks, scalar keys and items, skip, invalidation,
  removal and readdition, written operation order, and malformed marker placement
- provider-defined children accessed through built-in collections
- provider time series literal application through erased outputs, runtime registration precedence,
  nested publication, unsupported destinations, extension-defined sentinel instances,
  tagged sentinel arguments, and provider scalar construction inside scalar time series

### Nodes and graphs

For each reusable node and graph, test:

- output values and update selection
- invalidation and revalidation
- declared guarantees
- generic and resolved types
- supported scalar and time series types
- supported graph shapes
- reported errors

### Standard library

- validity queries at startup and on validity changes; immediate collection
  validity, empty collections, references, and free collections
- signal-driven complete-value sampling, passive value inputs, preserved
  outputs on invalidation, collection reconciliation, and repeated sampling of
  const collection items
- exact deduplication, output guarantees, unchanged guaranteed inputs without
  added nodes, reference-observed guarantees, and invalidation/revalidation
- conditional filtering, reopening after missed changes and removals,
  invalidation, revalidation, and recursive output guarantee limits
- whole-input reference selection, selected-reference retargeting, unchanged
  selections, and unselected-input updates
- arithmetic across scalar and time series arguments, exact type matching,
  integer and temporal wraparound, and borrowed string construction
- conversion between scalar time series, full target type matching including
  guarantees and collection results, reference-observed guarantees, and
  rejection of unsupported targets
- optional addition and multiplication, initial invalidity, invalidation,
  revalidation, startup scheduling, and distinctions from explicit defaults
- stateful count and one-event lag, including repeated ticks, validity changes,
  owned scalar snapshots, guarantees, references, and nested graph instances
- running sums, extrema, and means, including skipped cycles, repeated values,
  invalidation, reset-only and simultaneous reset/value ticks, reset invalidation,
  numeric limits, NaN comparisons, guarantees, references, and independent nested graph state
- collection sums, extrema, and means, including `opt` overload selection,
  partial validity, empty results, item removal and replacement, integer limits,
  reference rebinding, free collections, output guarantees, and equal optional
  sums across invalid-to-valid transitions
- delayed ticks and invalidation, collection delta replay, periodic tick limits,
  throttle publication times and latest-state reconciliation, simultaneous
  timers and input changes, duration bounds, and nested-graph stop cleanup
- scalar equality through built-in, compound, collection, and extension views;
  ordering and Boolean operations
- guarantee propagation, equal-result tick selection, reference bindings,
  scalar-only first-cycle evaluation and compiled node counts, and nested graph composition
- C++ and Python wiring syntax, named arguments, diagnostics, lifetime checks,
  native operator exports, and installed library use
- Boolean graph operators, required-input gating, evaluation of both C++ operand
  expressions, rejection of native truth conversion, and explicit output-presence checks

### Python integration

- homogeneous scalar tuple annotations, nested and generic items, immutable
  Python materialization, expected-type sequence conversion and ownership,
  and rejection of mutable sequence annotations
- fixed-length scalar tuple annotations, empty and single-item tuples, nested
  and generic items, positional conversion, arity errors, and immutable results
- declaration metadata lifetime after definition destruction and callback ownership
- scalar-or-time-series node input resolution and concrete binding
- wiring failures preserving Python exception identity, type, traceback, and context notes
- runner engine selection, default and explicit start times, simulation-only test capture,
  profiling, explicit cancellation, stop-state lifetime, and GIL release while waiting
- Python signal processing during idle realtime waits, future-start waits, and native evaluation;
  Ctrl+C cleanup, retained-writer closure, and custom signal-handler behavior
- callback-scoped wall-clock scheduling, timer firing, and cancellation
- queue-node scalar wiring, start and stop callbacks, injected handles, and output validation
- queue delta conversion, standard and eliding modes, batch element conversion, and collection removals
- copied Python writers, concurrent submission, realtime wakeups, and closed-handle rejection before conversion
- queue payload ownership, conversion failures, startup failures, and unread-message cleanup

### Performance and portability

- public and runtime object sizes
- graph and global-state allocation accounting
- compiled and runtime storage size
- scheduling, evaluation, reference, and collection benchmarks
- supported compiler builds and sanitizer runs

Build the `benchmark` preset and run `build/benchmark/benchmarks/bg_container_benchmark`
to compare the custom hash dictionary with the standard library implementation.
The [high-count map benchmark](../benchmarks/high_count_map.md) defines the
portable map workload and the format for commit-keyed comparison results.
The [evaluation context benchmark](../benchmarks/evaluation_context.md) measures
child-context construction and flat and nested evaluation that reads evaluation time.
