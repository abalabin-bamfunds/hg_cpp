# hgraph nested graph pattern findings

This review selects higher-level graph compositions from hgraph 0.5 and 0.8
tests and checks whether BG has corresponding behavior tests. It adds regression
tests only. It does not change engine code.

The [latest BG recheck](hgraph-pattern-recheck-2026-10-30.md) supersedes the
failure statuses below: the `take` composition and switch/mesh/reduce cases now
pass in the ordinary focused run. The two switch/mesh cases pass after adding
concrete `.as<TS<i64>>()` return annotations to their callbacks. The explicit
nested REF result boundaries still fail during wiring.

The source snapshots are hgraph 0.5 at `c34dba1d` and hgraph 0.8 at
`b9305bb2`. Final validation uses BG `8058a013`; full source revisions
are recorded in the [review summary](hgraph-pattern-review-2026-10-28.md).

## Classification

| Pattern | hgraph evidence | BG coverage | Status |
| --- | --- | --- | --- |
| Mapped reductions with switch selection and removal/recreation | 0.5 [`test_reduce_map_and_switch`](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph_unit_tests/_wiring/test_reduce.py); 0.8 [`test_tsd_reduce_wires_switch_inside_mapped_values`](https://github.com/hhenson/hgraph/blob/b9305bb23f8e54337cd1941447b940e5fd43ec59/python/tests/ported/_wiring/test_reduce.py) | [`mapped reductions apply each parent's switch to its own total across removal and recreation`](../../../tests/evaluation/reduce_test.cpp) | Added, passing. |
| Switch branches consume reductions, including empty input | 0.5 [`test_switch_from_reduce`](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph_unit_tests/_wiring/test_switch.py), [`test_switch_bundle_from_reduce`](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph_unit_tests/_wiring/test_switch.py); 0.8 [`test_switch_from_reduce`](https://github.com/hhenson/hgraph/blob/b9305bb23f8e54337cd1941447b940e5fd43ec59/python/tests/ported/_wiring/test_switch.py), [`test_switch_bundle_from_reduce`](https://github.com/hhenson/hgraph/blob/b9305bb23f8e54337cd1941447b940e5fd43ec59/python/tests/ported/_wiring/test_switch.py) | [`switch branches consume a reduced TSD value`](../../../tests/evaluation/switch_test.cpp), [`switch passes a reduced value through a free bundle argument`](../../../tests/evaluation/switch_test.cpp), [`an empty reduction uses its default again after an outputless switch branch`](../../../tests/evaluation/reduce_test.cpp) | Added, passing. |
| Mesh branches use dependencies and reduce nested outputs through parent changes | 0.5 [`test_mesh`](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph_unit_tests/_wiring/test_mesh.py), [`test_mesh_2`](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph_unit_tests/_wiring/test_mesh.py), [`test_mesh_removal`](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph_unit_tests/_wiring/test_mesh.py) | [`reduce combines mesh outputs as branches update and disappear`](../../../tests/evaluation/mesh_test.cpp), [`mesh branches reduce nested dictionaries before using their dependencies`](../../../tests/evaluation/mesh_test.cpp), [`mapped meshes reduce independently and rebuild after a parent returns`](../../../tests/evaluation/mesh_test.cpp), hidden repro [`switch-selected mesh branches feed a reduction`](../../../tests/evaluation/mesh_test.cpp), hidden repro [`a reduction observes mesh dependencies after switch outputs change`](../../../tests/evaluation/mesh_test.cpp) | Added from 0.5 patterns and extended with combinations. The two switch-containing mesh tests still fail during wiring with `type variable has no type map entry` after adding typed graph boundaries; they are hidden repros and establish no runtime behavior. No corresponding mesh test file was found in the pinned 0.8 ported tests. |
| Nested switch branches inside a map feed `take` while mapped references change | 0.5 [`test_cascaded_switch_in_map_ticking_into_take`](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph_unit_tests/_wiring/test_reference_observer_lifetime.py); 0.8 [same test](https://github.com/hhenson/hgraph/blob/b9305bb23f8e54337cd1941447b940e5fd43ec59/python/tests/ported/_wiring/test_reference_observer_lifetime.py) | Hidden repro [`take keeps the initial map of branches across nested switch replacement`](../../../tests/library/tick_selection_test.cpp) | BG rejects the inner switch while wiring: its `true` case returns a TSD reference and its `false` case returns an owned empty TSD. The error propagates through `build_take_frame` and the outer switch. The graph never evaluates, so this does not establish a runtime reference-lifetime failure. Run with Catch2 filter `bg_library_tests '[.][repro]'`. |

The 0.8 reduce suite also includes
[`test_switch_reduce_map_shrink_with_trace`](https://github.com/hhenson/hgraph/blob/b9305bb23f8e54337cd1941447b940e5fd43ec59/python/tests/ported/_wiring/test_reduce.py).
This review covers observable switch/map/reduce output changes; it does not
reproduce hgraph's per-node trace output.

The empty/singleton reduction contracts differ. BG forwards one valid item
unchanged and uses its named default only when no valid items remain. hgraph
0.8's `zero` argument also participates in a singleton reduction, as exercised
by [`test_tsd_reduce_zero_is_used_only_for_empty_and_singleton`](https://github.com/hhenson/hgraph/blob/b9305bb23f8e54337cd1941447b940e5fd43ec59/python/tests/ported/_wiring/test_reduce.py).
The new BG tests use BG's existing default contract and do not claim parity for
that behavior.

## Verification

Ten new ordinary nested-composition cases pass. Three hidden reproductions
retain their expected output assertions. The two mesh/switch reproductions
fail during wiring with `type variable has no type map entry`, including with
typed graph boundaries. The cascaded-switch reproduction fails during wiring
because its cases return incompatible TSD representations. These failures do
not establish runtime behavior. See the
[review summary](hgraph-pattern-review-2026-10-28.md) for complete-suite validation.

Run the reproductions from the review lane:

```sh
build/verification/bg_tests "switch-selected mesh branches feed a reduction"
build/verification/bg_tests "a reduction observes mesh dependencies after switch outputs change"
build/verification/bg_library_tests \
  "take keeps the initial map of branches across nested switch replacement"
```

TSD root invalidation is not supported through output APIs. The new empty-input
case checks an empty reduction across an outputless switch branch and its
reactivation. This review does not authorize production fixes.
