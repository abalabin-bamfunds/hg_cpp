# hgraph dispatch composition sweep, 2026-11-03

This sweep checks runtime dispatch inside map and switch graphs, per-key state,
structured outputs, and inherited context. It adds four tests and records two
API limitations. No production code was changed. Integration uses HG baseline
`54cdfda71`.

## Sources and existing coverage

The source snapshots are hgraph 0.5.42 at
`c34dba1db8a1c873ee303796db4c8fc48c8cc5be` and the 0.8 development snapshot at
`d36be500ec8b6fda77ff077566f891ffcf5c84a8`. Sources were read from committed
revisions.

- `hgraph_unit_tests/_wiring/test_dispatch.py` in 0.5 checks concrete and inherited
  type selection, multiple selected arguments, and unchanged branch selection.
- `tests/cpp/test_dispatch.cpp` in 0.8 checks composed structural branch outputs,
  enclosing context, selected reference inputs, and alternating branches.
- `python/tests/test_dispatch_covariant_output.py` in 0.8 checks scalar output
  covariance and Python-specific branch recreation and field adaptation.

Existing HG dispatch tests cover direct selection, scalar output covariance,
reference inputs, ambiguity, and fallback. This batch adds compositions with
map key lifecycle, stateful branch lifetimes, native TSB fields, and switch
context propagation.

## Added coverage

[`dispatch_composition_patterns_test.cpp`](../../../tests/evaluation/dispatch_composition_patterns_test.cpp)
contains four cases:

- Two mapped keys independently select base and derived implementations across
  type changes, removal, and recreation.
- Stateful implementations retain their counters on same-type updates. Type
  changes and map recreation start new branch state. Quiet cycles produce no
  output, and the sibling key retains its own state.
- Native TSB branch outputs retain the declared schema. A score-only update
  emits only the score field; branch replacement samples both current fields.
- Dispatch inside a switch receives the enclosing context through updates and
  switch branch replacement.

All four cases pass. Combined verification is recorded in the
[mesh sweep report](hgraph-mesh-settling-sweep-2026-11-03.md#integration-verification).

## Findings

### Free bundle output: documented dispatch restriction

The [dispatch contract](../design/operators.md#runtime-dispatch) requires
collection outputs to have identical dereferenced types. A free
`bundle<TSB<...>>` has reference fields, while a native `Out<TSB<...>>` has
value fields. Their types are compatible but differ after dereferencing the
collection root.

`DispatchBranchTemplate::wire` in [`dispatch.cpp`](../../../src/dispatch.cpp)
first checks compatibility, then rejects this collection type difference with
`dispatch output covariance requires scalar time-series values`.
`WiringPoint::as` in [`graph_wiring.cpp`](../../../src/graph_wiring.cpp) retains
an already compatible free bundle, so `.as<TSB<...>>()` does not materialize it.
The native TSB-producing node control passes and is used in the added test.

Classification: a documented collection output restriction. Generalizing this
boundary requires a separate design decision. No runtime defect was reproduced.

### Callable node state: C++ API implementation gap

The [node API](../design/node-api.md#private-state) permits a plain function to
request `State<T>` as a parameter. This declaration fails to compile:

```cpp
node([](In<TS<Bundle<DispatchPatternBase>>>, State<i64> evaluations) {
    return ++*evaluations;
});
```

`callable_fields_supported` in
[`callable_signature.hpp`](../../../include/hg/node/details/callable_signature.hpp)
accepts input, output, promoted, and scalar fields, but excludes state fields.
The stateful dispatch test uses aggregate node definitions, which support
`State<i64>` and pass the runtime trace.

Classification: a C++ declaration implementation gap against the documented
node API, independent of dispatch runtime behavior. No fix is included.

## Scope limits

This batch does not add Python reflection or overload-recreation behavior, or
collection output covariance beyond the existing dispatch contract. It does
not exhaust dispatch combinations with every higher-order graph or collection
shape.
