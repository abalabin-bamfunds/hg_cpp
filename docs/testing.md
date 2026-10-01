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
  `time-series-storage`, `time-series-view`, `collection-change`, `reference`, `sampling`, `service`, `getitem`, `getattr`,
  `nested-graph`, `mesh`, `multiservice`, or `variadic-wiring`
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
- `validity`: `required` or `optional`
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

With Python 3.10 or newer, build the `test-coverage-report` target to validate
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
- buffer allocation, contiguous chunks, chain rollover, concurrent reservations, ordered commit visibility, and ring backpressure
- constant-size scalar value handles
- visitor dispatch for concrete and generic scalar and time series views, including base-interface fallback, no
  matching overload, external scalar dispatch, runtime-type precedence, and recursion through scalar collections
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
- type variables, constraints, repeated variables, and lower bounds
- type-map overlays and functional variable resolution
- signature matching, resolution, and parameter category inference
- hierarchical scalar inheritance and directional compatibility through scalar containers
- operator signature specificity and ambiguity
- types passed as scalars
- guarantee compatibility, propagation, and enforcement
- collection types and nested types

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
- operator overload applicability, selection, requirements, resolvers, and caching
- context time series declaration, lexical binding, explicit arguments, type resolution, diagnostics, and caching
- graph arguments, including signature type resolution, repeated variables, exact and pattern matching,
  structure-based caching, and callable and aggregate graphs
- incompatible types, graph ownership errors, and invalid results
- variadic capture by TSB and homogeneous TSL, forwarding, materialization, exact-name direct collection binding,
  nested boundaries, and operator selection
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
- service binding metadata alongside inherited time series metadata

### Evaluation

- start, evaluation, and stop order
- one node evaluation per outer cycle
- scheduling during evaluation, including nodes in the current bit block
- active and passive inputs
- required-input validity caching, invalidation, and revalidation
- output update and invalidation propagation
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

### Nodes and graphs

For each reusable node and graph, test:

- output values and update selection
- invalidation and revalidation
- declared guarantees
- generic and resolved types
- supported scalar and time series types
- supported graph shapes
- reported errors

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
