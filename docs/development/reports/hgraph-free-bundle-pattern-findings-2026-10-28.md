# Free TSB and TSL patterns in hgraph tests

This report compares free bundle and fixed-list patterns in hgraph 0.5.42 and
0.8.37 with BG's graph-level tests. The review covers field/item references,
updates, validity, switch branches, and nested graph boundaries. It records
coverage and unresolved differences; it does not propose implementation fixes.

The initial BG baseline is `11ba59a4`; final validation uses `8058a013`. See
the [review summary](hgraph-pattern-review-2026-10-28.md) for build details. The 0.5
source is hgraph `v_0.5.42`, commit
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be).
The 0.8 source is hgraph `0.8.37`, commit
[`b9305bb23f8e54337cd1941447b940e5fd43ec59`](https://github.com/hhenson/hgraph/tree/b9305bb23f8e54337cd1941447b940e5fd43ec59).
The selected 0.5 tests use Python authoring; the selected 0.8 tests cover C++
runtime and wiring behavior. Findings compare the graph behavior represented by those tests,
not source syntax or internal implementation.

## Findings

| Source test | BG test or assessment | Classification |
| --- | --- | --- |
| 0.5 `hgraph_unit_tests/_wiring/test_switch.py::test_switch_bundle` | `a switch routes a free bundle between its fields and a constant bundle`; `a switch accepts native and free bundle branches` | Reproduced, but both cases fail; see [verified failures](#verified-failures). |
| 0.5 `hgraph_unit_tests/_wiring/test_tsl_wiring.py::test_fixed_tsl_non_peered_input`, `test_fixed_tsl_non_peered_input_generator`, `test_fixed_tsl_peered`, `test_peered_to_peered_tsl` | `a node can select an item from a free fixed time series list`; `a free fixed time series list coalesces source changes`; `a span-backed free TSL retains its items through materialisation and evaluation` | Covered for direct list construction, item projection, and source update coalescing. Generator-expression construction has no separate BG syntax and is not a runtime behavior gap. |
| 0.5 `hgraph_unit_tests/_wiring/test_tsl_wiring.py::test_tsl_ref_flipping` | `a free fixed list can be observed through a reference`; `reselecting a rebuilt free list keeps unchanged item references` | Partial. Reference observation passes; rebuilt-list reselection emits extra item ticks, confirmed on `8f0eaab6`. This exact source case selects among two lists and an empty reference using `combine[TSL]` and a dynamic index. That combination is not covered here. |
| 0.8 `tests/cpp/test_switch.cpp::switch_: a direct structural branch samples held bundle values on activation`; `switch_: a direct structural branch samples held list values on activation` | `a switch routes a free bundle between its fields and a constant bundle`; `a switch routes a free fixed list between its items and a constant list` | Reproduced, but both cases fail their expected structural tick results; see [verified failures](#verified-failures). |
| 0.8 `tests/cpp/test_switch.cpp::switch_: a pass-through branch publishes its input's own reference`; `switch_: a structural pass-through branch publishes the fields' own references` | `if_then_else preserves free bundle references across pass-through switch reselection`; `if_then_else preserves free fixed list references across pass-through switch reselection` | Added. The consumer reads one selected field/item and checks that toggling between the switch result and the original free structure produces no output while the selected source is unchanged. |
| 0.8 `tests/cpp/test_nested_wiring.cpp::nested wiring: a plain outer source adapts to a child REF input`; `nested wiring: a child REF output retargets an outer plain consumer`; `nested wiring: a REF pass-through samples and follows the plain parent source` | `a nested graph passes a free bundle reference to a value consumer`; `a nested graph passes a free fixed list reference to a value consumer` | Reproduced, but both cases fail during wiring; see [verified failures](#verified-failures). The failure does not establish that the pattern is invalid. |
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

## Verified failures

All seven reproductions below were rebuilt and rerun against BG `8058a013`.
They use hidden Catch2 tags `[.][repro]` and retain their original expected
results. Collection results in this table are captured tick deltas, not snapshots.

| BG test | Observed failure | Classification |
| --- | --- | --- |
| `a switch routes a free bundle between its fields and a constant bundle` | Captured deltas are `{count:1,value:2}`, `{}`, `{count:1,value:2}`, `{}`. The expected free-branch deltas contain held fields. | Collection observation gap: a scalar consumer reads the expected held values, but captured structural deltas omit them on activation. The graph sampling and capture paths need separate investigation. |
| `a switch accepts native and free bundle branches` | Wiring reports `switch case output types are incompatible`. | Mixed native/free TSB switch branch compatibility is unresolved. |
| `reselecting a rebuilt free bundle keeps unchanged field references` | Wiring reports `switch case output types are incompatible`. | Rebuilt free TSB switch branch compatibility is unresolved. |
| `a switch routes a free fixed list between its items and a constant list` | Captured deltas are `{0:1,1:2}`, `{}`, `{0:1,1:2}`, `{}`. The expected free-branch deltas contain held items. | Collection observation gap: a scalar consumer reads the expected held values, but captured structural deltas omit them on activation. The graph sampling and capture paths need separate investigation. |
| `reselecting a rebuilt free list keeps unchanged item references` | Freshly rechecked on `8f0eaab6`: captured output is `10, 10, 10, 20`; expected `10, SKIP, SKIP, 20`. | Confirmed failure. Both branches retain the same item time series. Rebuilding the container must not produce an item reference-change event. Keep the failing assertion hidden. |
| `a nested graph passes a free bundle reference to a value consumer` | Wiring reports `a time series reference cannot target another reference`. | TSB REF pass-through across a nested boundary remains a wiring repro. The design specifies reference-preserving REF-to-REF wiring; construction failure does not establish that the pattern is invalid. |
| `a nested graph passes a free fixed list reference to a value consumer` | Wiring reports `a time series reference cannot target another reference`. | TSL REF pass-through across a nested boundary remains a wiring repro. The design specifies reference-preserving REF-to-REF wiring; construction failure does not establish that the pattern is invalid. |

Scalar-consumer probes of the same free/constant switch constructions produce
`120, 90, 120, 270` for `100 * count + 10 * value` and
`102, 99, 102, 297` for `100 * first + second`. These probes distinguish
available held values from missing captured collection deltas; they do not
identify whether sampling or capture owns the omission.

The rebuilt-list case was rechecked with a rebuilt Release runtime and a fresh
list test translation unit after rebasing onto `8f0eaab6`. The exact command and
result log are in the [review summary](hgraph-pattern-review-2026-10-28.md#verification-boundaries).
The instrumented trace below identifies the item selection and binding path.

### Rebuilt-list item reference trace

Both branches resolve item 0 to the same scalar storage and runtime type.
The original source updates only in cycles 1 and 4. The trace names its storage
`S` and the selected item references `A`, `B`, and `C`:

| Cycle | Branch | Selected item reference | Source update time | Consumer input update time | Output |
| --- | --- | --- | --- | --- | --- |
| 1 | Pass-through | `A` | 1 | 1 | `10` |
| 2 | Rebuild | `B` | 1 | 2 | `10`, expected `SKIP` |
| 3 | Pass-through | `A` | 1 | 3 | `10`, expected `SKIP` |
| 4 | Rebuild | `C` | 4 | 4 | `20` |

Every selected reference resolves to `S`. Restarting the rebuilt branch creates
a new item selection in cycle 4.

The divergence starts in
[`ItemSelectionSubscriptions::require_selection`](../../../src/time_series/ref/item_selection.hpp).
For a value item it creates `ItemReference(collection.reference, id)`. Selecting
item 0 from the rebuilt list therefore creates `B` instead of forwarding `A`,
although both resolve to `S`. The free list exposes its input's scalar storage
and observed update time through `FreeFixedTimeSeriesListRuntimeType::item`.

`tsl_getitem_scalar_reference::evaluate` binds this selection and copies its
reference to the output. `DynamicReferenceInputStorage::refresh` compares the
reference handles. The changes `A` to `B` and `B` to `A` publish new reference
timestamps. `Node::binding_data` uses `ReferenceInputStorage::observed_state`,
which selects the later reference or target timestamp. The scalar consumer
therefore sees `modified() == true` in every cycle and returns `10` again in
cycles 2 and 3.

On cycle 2, activation also gives the rebuilt list's item input update time 2,
while the original source remains at time 1. On cycle 3, the selected target
again reports time 1, but the reference output's time 3 reaches the consumer.

The diagnostics were removed. Rebuilding the runtime and rerunning the original
test still produces `{10, 10, 10, 20}`. No production fix was made. The trace,
temporary diagnostic patch, and clean rerun are in the review lane:

- `build/verification/2026-10-29-free-list-consumer-trace.log`
- `build/verification/2026-10-29-free-list-instrumentation.patch`
- `build/verification/2026-10-29-free-list-clean-rerun.log`

The [nested reference contract](../design/time-series-references.md#nested-graph-boundaries)
permits an inner graph to return an input reference by copying it. The explicit
REF cases request this pass-through, not a reference targeting a reference.

Run the free-structure reproductions from the review lane:

```sh
build/verification/bg_tests '[repro][series:tsb],[repro][series:fixed-tsl]'
```

This command intentionally reports seven failed cases. The eleven new ordinary
free-structure cases pass in the complete suites. Each test retains the
required assertion or propagates the wiring exception.

## Coverage limits

The passing free-structure tests exercise source updates, skipped ticks,
partial invalidation, revalidation, reference conversion, and nested boundaries.
The switch and nested REF cases listed under verified failures do not establish
their intended behavior. The passing tests do not establish behavior for all
possible nesting depths, reference chains, or branch lifecycle combinations.

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
