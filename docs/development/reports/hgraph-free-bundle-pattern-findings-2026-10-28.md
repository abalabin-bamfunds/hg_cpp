# Free TSB and TSL patterns in hgraph tests

This report compares free bundle and fixed-list patterns in hgraph 0.5.42 and
0.8.37 with BG's graph-level tests. The review covers field/item references,
updates, validity, switch branches, and nested graph boundaries. It records
coverage and unresolved differences; it does not propose implementation fixes.

The BG test baseline is `11ba59a41a3b093024467cf6c2ef7cbd509d52b4`. The 0.5
source is hgraph `v_0.5.42`, commit
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be).
The 0.8 source is hgraph `0.8.37`, commit
[`b9305bb23f8e54337cd1941447b940e5fd43ec59`](https://github.com/hhenson/hgraph/tree/b9305bb23f8e54337cd1941447b940e5fd43ec59).
The 0.5 tests are Python authoring tests; the 0.8 tests are C++ runtime and
wiring tests. Findings compare the graph behavior represented by those tests,
not source syntax or internal implementation.

## Findings

| Source test | BG test or assessment | Classification |
| --- | --- | --- |
| 0.5 `hgraph_unit_tests/_wiring/test_switch.py::test_switch_bundle` | `a switch routes a free bundle between its fields and a constant bundle`; `a switch accepts native and free bundle branches` | Covered. The tests observe alternating free/native structural values and a constant branch. |
| 0.5 `hgraph_unit_tests/_wiring/test_tsl_wiring.py::test_fixed_tsl_non_peered_input`, `test_fixed_tsl_non_peered_input_generator`, `test_fixed_tsl_peered`, `test_peered_to_peered_tsl` | `a node can select an item from a free fixed time series list`; `a free fixed time series list coalesces source changes`; `a span-backed free TSL retains its items through materialisation and evaluation` | Covered for direct list construction, item projection, and source update coalescing. Generator-expression construction has no separate BG syntax and is not a runtime behavior gap. |
| 0.5 `hgraph_unit_tests/_wiring/test_tsl_wiring.py::test_tsl_ref_flipping` | `a free fixed list can be observed through a reference`; `reselecting a rebuilt free list keeps unchanged item references` | Partial. BG tests structural reference observation and reselection, but this exact case selects among two lists and an empty reference using `combine[TSL]` and a dynamic index. That combination is not covered here. |
| 0.8 `tests/cpp/test_switch.cpp::switch_: a direct structural branch samples held bundle values on activation`; `switch_: a direct structural branch samples held list values on activation` | `a switch routes a free bundle between its fields and a constant bundle`; `a switch routes a free fixed list between its items and a constant list` | Covered. Each branch change checks source values held across skipped source ticks, including a constant structural branch. |
| 0.8 `tests/cpp/test_switch.cpp::switch_: a pass-through branch publishes its input's own reference`; `switch_: a structural pass-through branch publishes the fields' own references` | `if_then_else preserves free bundle references across pass-through switch reselection`; `if_then_else preserves free fixed list references across pass-through switch reselection` | Added. The consumer reads one selected field/item and checks that toggling between the switch result and the original free structure produces no output while the selected source is unchanged. |
| 0.8 `tests/cpp/test_nested_wiring.cpp::nested wiring: a plain outer source adapts to a child REF input`; `nested wiring: a child REF output retargets an outer plain consumer`; `nested wiring: a REF pass-through samples and follows the plain parent source` | `a nested graph passes a free bundle reference to a value consumer`; `a nested graph passes a free fixed list reference to a value consumer` | Added. Each child accepts a reference to a free structure, returns it, and an outer value consumer observes independent source updates. |
| 0.8 `tests/cpp/test_nested_wiring.cpp::nested wiring: a structural initializer boundary binds leaf-wise into the child` | `a nested graph observes independent updates to free bundle fields`; `a nested graph observes independent updates to a free fixed list` | Partial. BG passes a typed free TSB/TSL through a nested graph and checks independent leaf updates. The hgraph test constructs the child's fixed TSL input from an initializer list of separate sources; the BG test supplies an explicit `tsl(...)` structure. |

The new graph tests are in
[`time_series_bundle_test.cpp`](../../../tests/evaluation/time_series/time_series_bundle_test.cpp)
and
[`time_series_list_test.cpp`](../../../tests/evaluation/time_series/time_series_list_test.cpp).
The switch identity cases are in
[`stream_test.cpp`](../../../tests/library/stream_test.cpp) because
`if_then_else` belongs to the library test target. Existing TSB/TSL cases remain
in their series test files, with switch and nested cases grouped after the
direct cases.

## Coverage limits

The free-structure tests exercise source updates, skipped ticks, partial
invalidation, revalidation, reference conversion, direct switch routing, and
one nested boundary. They do not establish behavior for all possible nesting
depths, reference chains, or branch lifecycle combinations.

The nested partial-validity and independent item-update cases are BG coverage
additions. The reviewed 0.8 nested-wiring test file does not contain a direct
partial-validity equivalent.

The 0.5 `test_tsl_ref_flipping` pattern remains unverified as a combined case.
BG has `combine` and collection selection operators, and separate list
reference tests, but this pass did not wire their exact sequence of two
reference-bearing lists and an empty reference. No behavior change is proposed
from that omission.

The source contains tests for features outside this review, including dynamic
lists and lower-level endpoint identity. Those patterns are not counted as
gaps unless they combine a free TSB/TSL with the boundaries covered here.
