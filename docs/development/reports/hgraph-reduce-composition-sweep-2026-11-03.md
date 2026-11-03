# hgraph reduce composition sweep, 2026-11-03

This batch adds three tests for nested dictionary reducers, bundle defaults, and
mapped reducer branch lifetime. It uses the refreshed pattern-review runtime at
`53194f8d6`. Changes are limited to tests and this report; no production code
changed.

## Sources and existing coverage

The upstream pins are hgraph 0.5.42 and the 0.8 development snapshot listed in
the [service composition sweep](hgraph-service-composition-sweep-2026-11-03.md#sources).
The 0.5 sources are `hgraph_unit_tests/_wiring/test_reduce.py` and
`test_switch.py`. The 0.8 sources are `python/tests/ported/_wiring/test_reduce.py`,
`test_switch.py`, `python/tests/test_reduce_zero_semantics.py`, and
`tests/cpp/test_reduce.cpp`.

| Source pattern | Existing HG coverage |
| --- | --- |
| 0.5 `test_reduce_map`; 0.8 `python/tests/test_reduce_zero_semantics.py::test_reduce_mapped_bundle_collection_projection_dereferences_key_set_source` | [`reduce maps and combines nested dictionaries`](../../../tests/evaluation/reduce_test.cpp) and [`reduce maps a projected bundle of nested dictionaries`](../../../tests/evaluation/reduce_test.cpp) cover nested dictionary reducers and bundle projection. |
| 0.8 `python/tests/ported/_wiring/test_reduce.py::test_switch_reduce_map_shrink_with_trace` | Existing tests cover smaller nested reduce and map/switch/reduce traces. The source exercises a 17-item reduction tree while the outer TSD shrinks, empties, and rebuilds. It also checks per-node trace output, which this test pack does not reproduce. |
| 0.5 and 0.8 `test_switch_bundle_from_reduce` | [`switch passes a reduced value through a free bundle argument`](../../../tests/evaluation/switch_test.cpp) covers a bundle-shaped reducer result crossing a switch boundary. Existing reduce tests cover scalar and non-scalar defaults, but not a TSB default updated while the TSD is empty. |

HG's reduction behavior follows its [reduce contract](../design/reduce.md).
The upstream tests select graph shapes and lifecycle transitions; their empty,
singleton, and trace expectations are not copied where they differ from HG's
contract.

## Added coverage

[`reduce_composition_patterns_test.cpp`](../../../tests/library/reduce_composition_patterns_test.cpp)
contains these cases:

| Case | Behavior checked |
| --- | --- |
| Nested dictionaries merge by key, then reduce their values | The mapped reducer combines overlapping inner keys and retains keys that occur in only one outer item. A second reduction produces the scalar total. |
| Bundle default applies only while the TSD has no valid items | Empty, singleton, pair, and odd-sized inputs are followed by removals. Updates to the free TSB default reach the output while the TSD is empty. |
| Nested dictionary reducer branches retire as the outer tree shrinks | Seventeen outer items shrink to six, grow to seven, then shrink to one and empty. New items recreate the tree before its final removal. Direct and switch-selected runs assert the same scalar trace. |

The higher-count case checks observable output and branch selection. It does not
assert hgraph's internal reducer trace. The selected switch remains stable on the
last removal so the final reduction output is observed after the selected branch
has processed the input.

## Findings and verification

All three cases pass: **3 cases and 4 assertions**. The direct `add_values` node
cannot serve as the reducer because the reduce operator requires reducer inputs
to be time series. The tests retain a small graph adapter with `TS<i64>` inputs.
Runner inputs and the scalar reducer output infer their types from graph
signatures, so they need no explicit `input<T>()` or `.as<T>()`. The bundle
default case keeps `.as<TSB<ReducedPair>>()` to make field projection's bundle
type explicit.

The focused test executable was built against the refreshed review headers and
Release libraries and run from `build/reduce-composition/cmake`. The full HG
suite was not run. No new failure or production defect was found. The source
is registered in `hg_library_tests` and this report is linked from the
development index.

## Scope limits

This sweep does not port all upstream reducer tests. `test_reduce_tuple` is an
ordered fold over a scalar tuple stream with a differently typed time-series
seed, outside the supported TSD reduction contract. Fixed-list higher-order
reduction is also outside the documented `reduce` input surface. Existing
coverage for map/reduce, switch/reduce, bundle projection, and scalar defaults
remains in place; this batch adds only the combinations listed above.
