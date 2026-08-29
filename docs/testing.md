# Testing

## Purpose

The test suite verifies graph correctness, supported interfaces, and performance
constraints. Tests use the lowest interface that contains the behavior under
test. Node and graph behavior tests use `test_graph`.

## Coverage model

Consider each feature across these dimensions:

- scalar or time series type
- concrete or generic type declaration
- graph shape and nesting depth
- input activity and validity constraint
- time series guarantee
- lifecycle stage and state transition
- C++ or Python definition
- storage representation

Select combinations that exercise distinct behavior. Add a combination for a
new code path, compatibility boundary, failure mode, or regression.

## Test layout

- `tests/utils`: independent utilities
- `tests/value`: scalar and time series value access and storage
- `tests/type`: type matching and resolution
- `tests/wiring`: graph construction and wiring-template calls
- `tests/graph`: graph compilation and compiled layout
- `tests/evaluation`: evaluation context, engines, lifecycle, scheduling, storage, time series, and validity
- `tests/node`: node declaration and invocation interfaces
- `tests/testing`: test utilities
- `tests/nodes`: reusable node behavior
- `tests/graphs`: reusable graph behavior

Name a test file after the feature it covers. Split node and graph behavior by
value type when the type changes the expected behavior. Keep each test case
focused on one behavior. Define a test-only function, node, or graph in the test
case that uses it. When an interface requires a namespace-scope type, place its
declaration immediately before the test case. Keep other declarations at file
scope only when multiple test cases share them.

`test_graph` accepts a wiring template or a graph function. Each input range
defines updates by outer cycle. An empty `std::optional` skips its cycle. The
result has one position per cycle and uses an empty position when the output did
not update.

## Test areas

### Utilities

- compile-time strings, symbols, named arguments, and reflection
- constant-size polymorphic values
- visitor dispatch for concrete and generic scalar and time series views, including base-interface fallback and no
  matching overload
- thread-local context storage
- scheduling bit iteration
- stable-address storage, shared occupancy, and incremental hash containers

### Value system

- canonical scalar conversion
- borrowed and owning scalar lifetime
- typed and erased scalar access, casts, equality, and hashing
- fixed, variable, inline, and graph-accounted storage
- construction, assignment, copy, and destruction
- opaque values and scalar collections, including generic and nested types
- time series validity, update time, mutation, and invalidation
- global state ownership, replacement, lookup, and allocation

### Type system

- resolved type identity and scalar or time series classification
- type variables, constraints, repeated variables, and lower bounds
- type-map overlays and functional variable resolution
- signature matching, resolution, and parameter category inference
- types passed as scalars
- guarantee compatibility, propagation, and enforcement
- compound and hierarchical scalar schemas, inheritance, and covariance
- collection types, nested types, and `valid_items`

### Wiring

- wiring-context nesting and thread isolation
- compile-time and runtime wiring-template invocation
- scalar and time series argument resolution
- optional scalar arguments and scalar ownership
- node-template and graph-template interchangeability
- graph-template boundaries and inner graph isolation
- wiring paths, labels, and diagnostics
- incompatible types, graph ownership errors, and invalid results

### Graph compilation

- topological order and cycle rejection
- node records, offsets, alignment, and size limits
- packed scalar, state, input, output, and view data
- runtime storage initialization by byte copy
- subscriber data for activity and validity changes
- start, stop, invalidation, and future-scheduling metadata
- repeated compilation and independent runtime graphs
- scalar and state destruction

### Evaluation

- start, evaluation, and stop order
- one node evaluation per outer cycle
- scheduling during evaluation, including nodes in the current bit block
- active and passive inputs
- required-input validity caching, invalidation, and revalidation
- output update and invalidation propagation
- invalidation behavior selected by a node
- future scheduling, cancellation, ordering, and coalescing
- simulation time progression and termination
- outer and inner graph failure behavior
- cycle callbacks and deferred storage reclamation
- global state shared by outer and inner graphs

### Time series references

- direct reference access and sampling
- value-to-reference and reference-to-value bindings
- reference changes and value observers
- dynamic subscribers and static subscriber entries
- references across inner graph boundaries
- dynamic collection items and outer-cycle removal lifetime
- representation creation and reuse

### Collection time series

- set additions and removals
- fixed and sparse list updates
- bundle fields and inheritance
- dictionary key, item, and validity changes
- nested dictionary representations and shared key sets
- output mutation paths and parent notifications
- tabular buffers and Arrow-compatible views

### Nodes and graphs

For each reusable node and graph, test:

- output values and update selection
- invalidation and revalidation
- declared guarantees
- generic and resolved types
- supported scalar and time series types
- supported graph shapes
- reported errors

### Python

- C++ and Python type and signature parity
- Python node and graph definitions
- scalar and time series conversion
- object lifetime and exception translation

### Performance and portability

- public and runtime object sizes
- graph and global-state allocation accounting
- compiled and runtime storage size
- scheduling, evaluation, reference, and collection benchmarks
- supported compiler builds and sanitizer runs

Build the `benchmark` preset and run `build/benchmark/benchmarks/bg_container_benchmark`
to compare the custom hash dictionary with the standard library implementation.
