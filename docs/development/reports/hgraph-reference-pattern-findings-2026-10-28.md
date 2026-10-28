# Reference and map patterns in hgraph tests

This report compares selected reference patterns in hgraph 0.5.42 and 0.8.37
with BG graph tests. It records coverage and unresolved behavior. It does not
propose implementation changes.

The review uses hgraph 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be)
and hgraph 0.8.37 at
[`b9305bb23f8e54337cd1941447b940e5fd43ec59`](https://github.com/hhenson/hgraph/tree/b9305bb23f8e54337cd1941447b940e5fd43ec59).
The BG branch is based on `8058a013`.

## Findings

| Source test | BG test or assessment | Classification |
| --- | --- | --- |
| 0.5 `hgraph_unit_tests/_wiring/test_map.py::test_map_re_added_references` | `map restores a removed key through a retained mapped reference` in [`map_test.cpp`](../../../tests/evaluation/map_test.cpp). The source graph has typed `TSS` and `TSD` parameters, maps values, then selects mapped values with the original key set. The BG case uses explicitly typed graph callbacks for both map layers. | Added, passing. Typed graph callbacks supply the required map inference. Generic callbacks fail during wiring with `argument does not satisfy its generic type`; this does not establish a runtime reference failure. |
| 0.8 `ported/_wiring/test_map_regressions.py::test_map_removed_typed_bundle_reference_does_not_retick_with_sibling` | `map drops a removed reference item while a sibling follows its target` in [`map_test.cpp`](../../../tests/evaluation/map_test.cpp). It uses a typed bundle value, removes one mapped reference, then updates the shared target through the retained sibling. | Added, passing. |
| 0.8 `ported/_wiring/test_map_regressions.py::test_map_preserves_typed_keyed_bundle_reference_and_materialization` | The same typed lookup and bundle shape is exercised by the test above. It checks reference output and sibling updates; it does not separately assert both source output forms (retained reference and explicit field materialization). | Partial. The source's static reference and materialization outputs are not both represented by this BG case. |
| BG combination of 0.5 `hgraph_unit_tests/_wiring/test_map.py::test_map_re_added_references`, 0.8 `python/tests/ported/_wiring/test_map_regressions.py::test_map_preserves_typed_keyed_bundle_reference_and_materialization`, and 0.8 `tests/cpp/test_nested_wiring.cpp::nested wiring: a child REF output retargets an outer plain consumer` | `repro: map retargets a selected dictionary reference across a nested graph` in [`map_test.cpp`](../../../tests/evaluation/map_test.cpp). No single upstream test contains this composition. The `as<TSD<i64, TS<i64>>>()` at the nested graph return supplies the target TSD type needed for map key and output inference. | Reproduced failure: captured deltas are `{1:1,2:2}`, `{1:11}`, `{1:3}`, `{1:4}`. The expected middle ticks are `{1:11,2:20}` and `{1:3,2:2}`. Held key 2 is absent on both target switches. The hidden case captures formatted output ticks. |
| BG combination of 0.5 `hgraph_unit_tests/_wiring/test_context.py::test_context_over_switch` and `test_context_over_switch_inside_map`, and 0.8 `python/tests/ported/_wiring/test_context.py::test_context_crosses_nested_map_and_switch_boundaries` | `a TSD item reference context reacts to selector and target changes` and `a switch does not retick a TSD item reference argument for an unchanged selector` in [`context_time_series_test.cpp`](../../../tests/evaluation/context/context_time_series_test.cpp). The BG cases add a TSD item reference as the context or switch argument and distinguish selector ticks from target ticks. | Added, passing. |
| BG combination of 0.5 `hgraph_unit_tests/_wiring/test_context.py::test_context_over_switch_inside_map`, and 0.8 `python/tests/ported/_wiring/test_context.py::test_context_crosses_nested_map_and_switch_boundaries` and `test_aliased_context_crosses_a_mapped_child_boundary` | `each mapped graph reads the context for its own key` in [`map_test.cpp`](../../../tests/evaluation/map_test.cpp). The selector and context update independently for each dictionary key. | Added, passing. No single upstream test contains this per-key selector and reference-backed context composition. |

## Verification

All five new ordinary reference/context cases pass against BG `8058a013`.
The hidden nested-retarget case fails its output assertion with the captured
trace above. See the [review summary](hgraph-pattern-review-2026-10-28.md)
for complete-suite validation.

Run the hidden reproduction from the review lane:

```sh
build/verification/bg_tests \
  "repro: map retargets a selected dictionary reference across a nested graph"
```

The nested-retarget reproduction retains its output assertion and stays
excluded from default test runs. `as<TSD<i64, TS<i64>>>()` is retained because
it supplies compile-time map inference; this is a wiring type requirement, not
an observed runtime defect.
