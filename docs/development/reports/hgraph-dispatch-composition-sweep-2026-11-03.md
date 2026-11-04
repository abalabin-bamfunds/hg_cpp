# hgraph dispatch composition sweep, 2026-11-03

This sweep checks runtime dispatch inside map and switch graphs, per-key state,
structured outputs, and inherited context. It fixes collection output
compatibility and records a separate callable node declaration gap.
Integration uses HG baseline `be009f7b3`.

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
covers these behaviors:

- Two mapped keys independently select base and derived implementations across
  type changes, removal, and recreation.
- Stateful implementations retain their counters on same-type updates. Type
  changes and map recreation start new branch state. Quiet cycles produce no
  output, and the sibling key retains its own state.
- Native TSB branch outputs retain the declared schema. A score-only update
  emits only the score field; branch replacement samples both current fields.
- Dispatch inside a switch receives the enclosing context through updates and
  switch branch replacement.
- Free and native TSB and fixed-list outputs retain held fields and items
  across branch replacement and later partial updates.
- Dictionary outputs accept reference items from map and value items from a
  direct branch through the same declared output type.

Combined verification is recorded in the
[mesh sweep report](hgraph-mesh-settling-sweep-2026-11-03.md#integration-verification).

## Findings

### Resolved: compatible collection outputs rejected by dispatch

The dispatch wrapper compared dereferenced collection type identities after
checking output compatibility. A free
`bundle<TSB<...>>` has reference fields, while a native `Out<TSB<...>>` has
value fields. Their types are compatible but differ after dereferencing the
collection root.

`DispatchBranchTemplate::wire` in [`dispatch.cpp`](../../../src/dispatch.cpp)
now returns compatible collection outputs directly. It retains the declared
output compatibility check and scalar output conversion. Switch removes
references recursively before computing its common output target, then
publishes a reference to that target, following the accepted
[switch output compatibility decision](../decisions/0094-switch-output-reference-compatibility.md).

Classification: a wiring defect, resolved by removing the redundant type
identity check. The new TSB and TSD cases reproduce the original rejection
with `dispatch output covariance requires scalar time-series values`. The
free/native fixed-list case also passes with the original check and provides
a control for held-item sampling across branch replacement.

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

This batch does not add Python reflection or overload-recreation behavior. It
does not exhaust dispatch combinations with every higher-order graph or
collection shape.
