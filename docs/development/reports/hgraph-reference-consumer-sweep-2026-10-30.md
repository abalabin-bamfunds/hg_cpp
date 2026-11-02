# hgraph reference-consumer sweep, 2026-10-30

This follow-up checks source switching and consumers of mapped references.
It uses BG `61574125` with the six tests from `b25ff057`, and the
upstream revisions pinned in the [preceding sweep](hgraph-pattern-sweep-2026-10-30.md).
No production code is changed.

The batch adds ten passing cases and four hidden reproductions of two findings:
mesh branch output inference fails during wiring, and dictionary selection
retains a valid item after its selected source removes that item.
Both findings were reproduced and traced on 2026-11-02 against the unchanged
BG baseline. Both are implementation bugs under the existing contracts.

## Source patterns

| Upstream source | BG composition |
| --- | --- |
| 0.5 `hgraph_unit_tests/_operators/test_tsd_operators.py::test_tsd_get_items_change_tsd`; 0.8 `python/tests/ported/_operators/test_tsd_operators.py::test_tsd_get_items_change_tsd` | Select keys after switching between dictionaries; repeat through an identity map. |
| 0.8 `python/tests/test_ref_consumer_sweep.py::test_ref_source_matches_plain`, `switch_flip` source | Replace switch branches whose REF outputs select the same outer TS, TSS, TSD, fixed TSL, or TSB; observe later target updates through ordinary value consumers. |
| 0.5 `hgraph_unit_tests/_wiring/test_map.py::test_map_input_rebind_to_nonpeer` | Switch from a direct dictionary to a mapped dictionary, then replace mapped item references before a downstream map consumes them. |
| 0.5 `hgraph_unit_tests/_wiring/test_map.py::test_map_reference_cleanup_2` | Keep a key-selected map branch alive across source removal, unrelated item updates, and source re-addition. |
| 0.8 `python/tests/test_ref_consumer_sweep.py::test_ref_source_matches_plain`, `map_nested` source with `self_keyed` and `reduce` consumers | Select a mapped dictionary using its own key set; reduce mapped scalar references across updates and membership changes. |
| The same `map_nested` source with the `mesh_` consumer | BG extension: map nested dictionaries by identity, reduce each dictionary inside a mesh branch, then reduce the mesh results. |

The five shape-specific switch cases are in
[`stream_test.cpp`](../../../tests/library/stream_test.cpp). Dictionary
selection, self-selection, and map rebinding are in
[`collection_test.cpp`](../../../tests/library/collection_test.cpp).
The higher-order consumers are in
[`reduce_test.cpp`](../../../tests/evaluation/reduce_test.cpp) and
[`mesh_test.cpp`](../../../tests/evaluation/mesh_test.cpp).

The source-switching cases use distinct values under equal keys so an incorrect
retained target cannot pass by value equality. The switch cases use explicit
REF-forwarding nodes in both branches. Their selector ticks independently of
the target. Assertions include quiet cycles as well as output values.

The map-reference cleanup case extends the upstream trace with re-addition of
the removed source key. The source-switching cases continue with an update
after the final switch to check that the selected source remains subscribed.

A dereferencing binding retains the new handle and samples only when the
resolved target changes. An explicit REF input receives normal reference ticks.
See the [reference binding contract](../design/time-series-references.md#subscriptions).

## Findings

### Mesh branch output inference

The new mesh composition fails during wiring when its branch returns an
inferred `reduce` output:

```cpp
const auto sum_inner = graph([](wp<TSD<i64, TS<i64>>> items) {
    return reduce(sum_mesh_values, items, "default"_n = i64{0});
});
```

The error is `type variable has no type map entry`. The branch inputs are
concrete; its returned wiring point has the unresolved C++ annotation `TS_T`.
The intended four-cycle output is `{7, 18, 9, 15}`. Evaluation does not start.

The same composition passes with an explicit branch output annotation:

```cpp
const auto sum_inner = graph([](wp<TSD<i64, TS<i64>>> items) -> wp<TS<i64>> {
    return wp<TS<i64>>{reduce(sum_mesh_values, items, "default"_n = i64{0})};
});
```

Classification: wiring implementation bug. The explicit-output case
establishes the mapped-reference runtime behavior.

[`GraphTemplate`](../../../include/bg/graph/graph_template.hpp) permits output
inference. Mesh wraps the branch in
[`ScopedWiringTemplate`](../../../include/bg/graph/details/scoped_wiring_template.hpp),
which inherits `allows_output_inference() == false` from `WiringTemplate`
instead of forwarding the target's policy. Signature resolution therefore
requires an output mapping before invoking the graph body. It cannot use the
body's returned wiring point to resolve `TS_T`, as required by the
[graph output-inference contract](../design/graph-wiring.md#graph-templates).

Mesh needs a known branch output type before wiring a `mesh()[key]` selection.
This branch has no dependency selection, so that
[mesh constraint](../design/mesh.md#purpose) does not require an output
annotation here.

### Selected-key invalidation after source switching

Both direct and identity-mapped selection omit an invalidation delta when a
selected key is absent from the newly selected source. The key remains in the
selected key set. BG's
[collection-selection contract](../design/library.md#collection-selection)
requires that key to remain present with an invalid reference.

At cycle 3, the test switches from the second source to the first source,
removes source key 1, and deselects key 2. The materialized output is
`{2: REMOVE}`; the expected delta is `{1: INVALID, 2: REMOVE}`.
Both source dictionaries remove key 1 in this cycle. Later re-addition and
source updates produce the expected deltas.

```text
expected: [SKIP, {1: 3}, {1: 40, 2: 20}, {1: INVALID, 2: REMOVE}, SKIP, {1: 7}, {1: 70}, {1: 80}]
actual:   [SKIP, {1: 3}, {1: 40, 2: 20}, {2: REMOVE},             SKIP, {1: 7}, {1: 70}, {1: 80}]
```

The scalar consumer
`valid(getitem(getitem(selected_source, selected_keys), 1))` also misses the
removal and re-addition. Its output is `{false, true, SKIP, SKIP, SKIP, SKIP,
SKIP, SKIP}` instead of `{false, true, SKIP, false, SKIP, true, SKIP, SKIP}`.
The expected trace follows the deduplicated Boolean contract of `valid`.

A temporary node with an independent cycle input inspected
`values[1].valid()` on every cycle. It observed
`[SKIP, true, true, true, true, true, true, true]`; the item should be invalid
at cycles 3 and 4. This confirms stale selected state or routing, rather than
only missing change notifications. The diagnostic was removed after this check;
the three retained reproductions assert collection deltas and scalar validity
through ordinary downstream consumers.

Classification: runtime implementation bug in
[`SampledDictionary::populate_changes`](../../../src/time_series/tsd/tsd_reference_sample.cpp).
At cycle 3 both sources remove key 1 before the selection node runs. The sample
enumerates only the currently live keys of the previous and current sources.
Key 1 is absent from both enumerations, so the sample never reports its removal.
The previous source's same-cycle removal information is available but unused.

The paired [`SampledSet`](../../../src/time_series/tss/tss_reference_sample.cpp)
reconstructs pre-cycle membership using same-cycle additions and removals.
The dictionary item comparison does not apply that rule. Its sampled key set
can therefore report a removal that its item delta omits.

[`tsd_getitem_tss`](../../../src/time_series/tsd/operators.cpp) invalidates
selected keys reported by `dictionary.for_each_removed`. That callback never
receives key 1, so the result retains its previous reference. The correction
belongs in dictionary sampling, where the transition from the previous
source's pre-cycle membership to the current source must be computed.

## Scope

These are selected products from the upstream reference-consumer sweep, with
explicit BG output assertions. They do not establish the complete Cartesian
product of upstream source, shape, and consumer combinations.

Higher-order map and reduce over fixed TSL remain outside BG's supported
surface. The fixed-TSL switch case uses an ordinary value consumer. Python
scalar conversions and aggregate conversion behavior are outside this C++
composition check.

The 0.8 operator test
`test_default_adapts_non_empty_tsd_to_unpartition_reference_leaves` requires
`default(unpartition(primary), fallback)` with a TSD fallback. BG declares
[`default_`](../../../include/bg/operators.hpp) as
`(TS<SCALAR>, SCALAR) -> TS<SCALAR>`. Collection inputs and time-series fallbacks
are unsupported, so this composition was skipped.

The attribute variant of 0.8
`test_nested_map_projection_preserves_final_key_removal` projects a bundle
field from the entire mapped dictionary. BG's
[`getattr` overloads](../../../src/time_series/tsb/operators.cpp) accept a TSB
or scalar Bundle, not `TSD<K, TSB<...>>`. This convenience form was skipped.
The explicit map projection is covered by the preceding sweep.

## Verification

The focused runners use GCC Release flags (`-O3 -DNDEBUG`) and the matching BG
runtime. The library runner covers `stream`, `collection`, `merge`,
`tsd_transform`, and `tick_selection`. The engine runner covers context time
series, map, mesh, reduce, switch, TSB, fixed TSL, and nested graphs.
The complete BG suite was not run for this tests-and-report change.

Independent final runs passed **501 cases and 886 assertions**:

| Runner | Passing cases | Passing assertions |
| --- | ---: | ---: |
| Library | 229 | 477 |
| Engine | 272 | 409 |

Explicit `[repro]` runs confirmed the three collection failures and the mesh
wiring failure above. Their assertions retain the required behavior. The
hidden tag excludes them from ordinary runs. Run both groups from the lane:

```sh
build/integration/hgraph-pattern-library-tests
build/integration/hgraph-pattern-engine-tests
build/integration/hgraph-pattern-library-tests '[repro]'
build/integration/hgraph-pattern-engine-tests '[repro]'
```

The four reproductions fail as documented. All fourteen new cases have valid
coverage tags. Library coverage-tag validation passes.

Engine coverage-tag validation reports four existing diagnostics in unrelated
switch and TSB tests: an undocumented `definition:graph` value, two uses of an
unknown `lifetime` dimension, and one test missing a `feature` tag. These tags
are present in the baseline. The existing invalid `error:operator-resolution`
tag in the touched stream file was changed to a plain `operator-resolution`
marker.
