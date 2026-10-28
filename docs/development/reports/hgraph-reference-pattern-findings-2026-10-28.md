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
| 0.5 `_wiring/test_map.py::test_map_re_added_references` | `repro: map makes a removed key available from a retained mapped reference` in [`map_test.cpp`](../../../tests/evaluation/map_test.cpp). The source graph has typed `TSS` and `TSD` parameters, maps values, then selects mapped values with the original key set. The BG reproduction now uses explicitly typed graph callbacks for both map layers. | Unverified after adding typed callbacks. An earlier generic-callback reproduction failed while wiring callback `[2]` with `argument does not satisfy its generic type`; that result is not treated as a runtime reference failure. Re-run the current typed-callback case before classifying it. |
| 0.8 `ported/_wiring/test_map_regressions.py::test_map_removed_typed_bundle_reference_does_not_retick_with_sibling` | `map drops a removed reference item while a sibling follows its target` in [`map_test.cpp`](../../../tests/evaluation/map_test.cpp). It uses a typed bundle value, removes one mapped reference, then updates the shared target through the retained sibling. | Added, passing in the earlier map-suite run. |
| 0.8 `ported/_wiring/test_map_regressions.py::test_map_preserves_typed_keyed_bundle_reference_and_materialization` | The same typed lookup and bundle shape is exercised by the test above. It checks reference output and sibling updates; it does not separately assert both source output forms (retained reference and explicit field materialization). | Partial. The source's static reference and materialization outputs are not both represented by this BG case. |
| 0.8 keyed dictionary reference selected through a nested graph and consumed by `map` | `repro: map retargets a selected dictionary reference across a nested graph` in [`map_test.cpp`](../../../tests/evaluation/map_test.cpp). The `as<TSD<i64, TS<i64>>>()` at the nested graph return supplies the target TSD type needed for map key and output inference. | Reproduced failure in the earlier run. The output trace was `{1:1,2:2}`, `{1:11}`, `{1:3}`, `{1:4}`. The corrected assertion requires `{2:20}` on the switch to the second dictionary and `{2:2}` when switching back to the first. Re-run on the latest runtime to confirm the trace. |
| Reference-valued context and argument through switch branches | `a TSD item reference context reacts to selector and target changes` and `a switch does not retick a TSD item reference argument for an unchanged selector` in [`context_time_series_test.cpp`](../../../tests/evaluation/context/context_time_series_test.cpp). | Added, passing in the earlier run. They cover selector changes separately from reference-target changes and check that an unchanged selector does not retick an argument. |
| A map of nested graphs with per-key reference-backed context | `each mapped graph reads the context for its own key` in [`map_test.cpp`](../../../tests/evaluation/map_test.cpp). The selector changes independently for each key while one context source updates. | Added, passing in the earlier run. This is a BG composition case; it is not a direct port of a single upstream test. |

## Verification

The earlier Release run, before rebasing and before the final callback and
assertion edits, passed the standard map suite with **73 cases and 151
assertions**. The two added context cases also passed. The final typed-callback
map test and corrected nested-retarget assertion passed C++ syntax checking;
their latest-runtime execution is pending.

Run the added passing cases and hidden reproductions from the review lane with
the latest runtime first on the library path:

```sh
LD_LIBRARY_PATH=/home/abalabin/_bg/.lanes/hgraph-pattern-review/build/verification/current-runtime \
  /home/abalabin/_bg/.lanes/hgraph-pattern-review/build/verification/bg_tests \
  "map drops a removed reference item while a sibling follows its target"

LD_LIBRARY_PATH=/home/abalabin/_bg/.lanes/hgraph-pattern-review/build/verification/current-runtime \
  /home/abalabin/_bg/.lanes/hgraph-pattern-review/build/verification/bg_tests \
  "repro: map makes a removed key available from a retained mapped reference"

LD_LIBRARY_PATH=/home/abalabin/_bg/.lanes/hgraph-pattern-review/build/verification/current-runtime \
  /home/abalabin/_bg/.lanes/hgraph-pattern-review/build/verification/bg_tests \
  "repro: map retargets a selected dictionary reference across a nested graph"
```

The hidden cases retain their output assertions and stay excluded from default
test runs. Update the classifications and verification result after the typed
reproduction runs. `as<TSD<i64, TS<i64>>>()` is retained because it supplies
compile-time map inference; this is a wiring type requirement, not an observed
runtime defect.
