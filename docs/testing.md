# Testing

## Purpose

The test suite verifies graph correctness, supported interfaces, and performance
constraints. Tests use the lowest interface that contains the behavior under
test. Node and graph behavior tests use `test_graph`.

## Coverage model

Structured Catch2 tags are the source of coverage information. Generated
coverage reports read these tags. Do not maintain a separate list of covered
combinations.

Use `[dimension:value]` for each applicable dimension:

- `feature`: `compilation`, `compilation-order`, `inherited-time-series`, `scalar-api`, `scalar-storage`,
  `scalar-collection`, `global-state`, `visitor`, `type-system`,
  `node-api`, `state`, `wiring`, `context-time-series`, `graph-compilation`, `test-graph`, `evaluation-context`,
  `evaluation-scratch`,
  `lifecycle`, `finalization`, `scheduling`, `timed-scheduling`, `simulation`, `wall-clock`, `input-activity`, `validity`,
  `invalidation`, `operator`, `constant`, `graph-argument`, `map`, `switch`,
  `time-series-storage`, `time-series-view`, `collection-change`, `reference`, `sampling`, `feedback`, `service`,
  `getitem`, `getattr`, `nested-graph`, `mesh`, `multiservice`, `variadic-wiring`, or `wiring-materialisation`
- `series`: `value`, `fixed-tsl`, `tss`, `tsd`, `tsb`, `reference`, or `generic`
- `schema`: `named`, `unnamed`, or `generic`
- `representation`: `native`, `free`, or `covariant`
- `scalar`: `byte`, `i64`, `integer`, `floating-point`, `boolean`, `string`, `symbol`, `evaluation-time`, `timedelta`,
  `opaque`, `compound`, `hierarchical`, `list`, `set`, `dict`, or `generic`
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

Name a test file after the feature it covers. Split node and graph behavior by
value type when the type changes the expected behavior. Keep each test case
focused on one behavior. Define a test-only function, node, or graph in the test
case that uses it. When an interface requires a namespace-scope type, place its
declaration immediately before the test case. Keep other declarations at file
scope only when multiple test cases share them.

`test_graph` accepts a wiring template or a graph function. Each input range
defines updates by outer cycle. An empty `std::optional` skips its cycle. The
result has one position per cycle and uses an empty position when the output did
not update. Pass a `ServiceCatalogue` after the tested wiring template or function
to finalize [graph services](services.md) before compilation.

`test_wiring` creates a `GraphWiring` and `WiringContext` for a `void` callable.
Use it when the behavior under test ends during wiring and does not require
graph compilation or evaluation.

## Test areas

### Utilities

- compile-time strings, symbols, named arguments, aggregate reflection, and C++ schema reflection and construction
- buffer allocation, contiguous chunks, chain rollover, concurrent reservations, ordered commit visibility,
  ring-full rollover, and allocation-failure rollback
- constant-size scalar value handles
- visitor dispatch for concrete and generic scalar and time series views, including base-interface fallback, no
  matching overload, external scalar and time series dispatch, runtime-type precedence, and recursion through collections
- thread-local context storage
- scope-bound cleanup and release
- scheduling bit iteration
- stable-address storage, shared occupancy, and incremental hash containers

### Value system

- canonical scalar conversion
- borrowed and owning scalar lifetime
- typed and erased scalar access, casts, equality, and hashing
- fixed, variable, inline, and graph-accounted storage
- construction, assignment, copy, and destruction
- opaque values and scalar collections, including generic and nested types
- compound scalar metadata, native access, generic field access, nesting, equality, hashing, and invalid declarations
- hierarchical scalar identity, schema metadata, inheritance, field narrowing, and container variance
- time series validity, update time, mutation, and invalidation
- global state ownership, replacement, lookup, and allocation

### Type system

- resolved type identity and scalar or time series classification
- process-owned dynamic declarations and type-pointer validity after graph or wiring-template destruction
- canonical factory cache identity, nested construction, failed factory retries, and concurrent cold misses
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
- operator signature specificity and ambiguity
- types passed as scalars
- guarantee compatibility, propagation, and enforcement
- collection types and nested types
- external declarations, public variable enumeration, runtime registration, and selection of supplied representations
- extension-owned configured output declarations, runtime selection, semantic compatibility, and preservation through invocation and invalidation
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
- `opt` argument matching, required-input preference, cache separation, named arguments, and variadic forwarding
- context time series declaration, lexical binding, snapshot capture and activation, explicit arguments, type
  resolution, diagnostics, and caching
- graph arguments, including signature type resolution, repeated variables, exact and pattern matching,
  structure-based caching, and callable and aggregate graphs
- graph signature types shared across different structures, constraint specificity, captured-value cache separation,
  and release of argument and overload constraint values
- incompatible types, graph ownership errors, and invalid results
- variadic capture by TSB and homogeneous TSL, forwarding, materialization, exact-name direct collection binding,
  nested boundaries, and operator selection
- deferred wiring points, retained arguments, advertised materialisation options, signature form matching and completion,
  deduplicated materialisation cost, callback non-execution during matching and root collection, shared concrete demands
  across Union and concrete consumers, retained context bindings, failed transitive collection, unforced expansion,
  lazy results, and expansion caching
- immediate validation before pending-call registration; deferred errors retaining consumer, parameter, collection-item,
  and service contexts; shared request origins; callback exception causes; diagnostic lifetime; offered-form and
  matched-binding costs without additional callback execution
- map argument selection, graph-function adaptation, overload selection, graph identity, and diagnostics
- mesh argument selection, dependency selection, output inference, overload selection, and diagnostics
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
- const nodes and their direct bindings
- `default_` adapter-node lowering, reference materialization, nested graph returns, and fallback identity
- service binding metadata alongside inherited time series metadata

### Evaluation

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
- retained tick writing and replay through a one-cycle delay node, current-cycle filtering, delta-only collection
  replay from a matching baseline, nested key changes, empty first ticks, validity transitions, payload cleanup,
  provider scalar handlers, and unsupported references
- feedback source and sink wiring, next-cycle replay, first ticks, validity changes, guarantee compatibility,
  separate handles, and same-graph binding
- `default_` fallback selection, timestamps, modified state, and source validity transitions
- invalidation behavior selected by a node
- timed scheduling, cancellation, ordering, and coalescing
- simulation time progression and termination
- system and injected wall-clock access during lifecycle, nested evaluation, invalidation, and failure cleanup
- outer and inner graph failure behavior
- cycle callbacks and deferred storage reclamation
- global state shared by outer and inner graphs
- context time series startup sampling, changes, validity, and use across nested graph boundaries
- mapped TSD unions, selected key sets, per-key inner graph evaluation, scheduling, and lifecycle
- mesh dependency creation, key changes, shared dependencies, cycle rejection, evaluation order, and lifecycle
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
- fixed-list updates
- bundle fields and inheritance
- dictionary key, item, and validity changes
- nested dictionary representations and shared key sets
- output mutation paths and parent notifications
- provider-defined children accessed through built-in collections

### Nodes and graphs

For each reusable node and graph, test:

- output values and update selection
- invalidation and revalidation
- declared guarantees
- generic and resolved types
- supported scalar and time series types
- supported graph shapes
- reported errors

### Python integration

- declaration metadata lifetime after definition destruction and callback ownership
- scalar-or-time-series node input resolution and concrete binding
- wiring failures preserving Python exception identity, type, traceback, and context notes

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
