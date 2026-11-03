# hgraph reference composition sweep, 2026-11-03

This batch adds higher-level graph compositions around selected references,
free fixed collections, and bundle fields. It extends the
[reference-consumer sweep](hgraph-reference-consumer-sweep-2026-11-02.md).
The HG baseline is `53194f8d6`. The source snapshots are hgraph 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be)
and the development snapshot used for 0.8 patterns at
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8).
Changes in this batch are tests and this report only.

## Sources and selected patterns

The new cases combine supported upstream patterns. They are not literal ports
of the upstream suites.

| Sources | Pattern reviewed |
| --- | --- |
| 0.5 `hgraph_unit_tests/_wiring/test_map.py::test_map_re_added_references`, `test_map_reference_cleanup`, and `test_map_input_rebind_to_nonpeer`; 0.8 `python/tests/ported/_wiring/test_map_regressions.py::test_map_generic_bundle_child_can_dereference_its_element` and `test_key_only_map_combines_captured_reference_fields` | Select mapped dictionary items, follow their target updates, and preserve reference fields across mapped bundle composition. |
| 0.5 and 0.8 `_operators/_conversion_operators/test_mapping_conversion_operators.py::test_convert_tsd_to_mapping` and `test_collect_mapping` | Convert dictionary time series to scalar snapshots and collect selected scalar events. |
| 0.5 `hgraph_unit_tests/_wiring/test_mesh.py::test_mesh` and `test_mesh_removal`; 0.8 `python/tests/test_mesh_generic_output.py` | Mesh branch output and generic output resolution. |
| 0.5 `hgraph_unit_tests/_wiring/test_tsl_wiring.py::test_fixed_tsl_non_peered_input` and `test_tsl_ref_flipping`; 0.8 `python/tests/ported/_wiring/test_tsl_wiring.py` and `_operators/_conversion_operators/test_tsl_conversion_operators.py` | Build a free fixed list from selected references and consume sparse item changes. |
| 0.5 `hgraph_unit_tests/_wiring/test_tsb_wiring.py::test_free_tsb_ref_signal` and `test_tsb_ref_flipping`; 0.8 `python/tests/ported/_wiring/test_tsb_wiring.py` | Recombine free bundle fields from references and observe partial field updates. |
| 0.5 `hgraph_unit_tests/_wiring/test_switch.py::test_nested_switch` and `test_switch_bundle`; 0.8 `python/tests/test_switch_reference_identity.py::test_a_switch_pass_through_branch_publishes_its_input_reference` | Switch an outer source before selecting a nested mapped item and check reference routing through branch changes. |

The source files above are read from the pinned commits. The 0.8 snapshot has
no direct ported mesh wiring test. The mesh reference-selection case extends
the 0.5 mesh pattern through HG's supported `mesh` API.

## Added coverage

The ten cases are in
[`reference_composition_patterns_test.cpp`](../../../tests/library/reference_composition_patterns_test.cpp).

| Composition | Assertions |
| --- | --- |
| Mapped dictionary item to target value | A selected nested item follows target updates. |
| `collect` over a nested mapped reference | Appends selected target events, ignores unrelated keys, and preserves history across invalidation and revalidation. |
| `merge` over two nested mapped references | Forwards updates from either selected item in input order. |
| Dictionary conversion after nested selection | Emits complete `Dict` snapshots after selected-item updates and removal; unrelated outer-key updates produce no output. |
| Compound scalar field through mapped selection | A field projection follows complete scalar updates. |
| `collect` over mesh item references | Accumulates selected mesh outputs as their items update. |
| Direct dictionary selection before conversion | Provides a control case that asserts complete `Dict` snapshots directly. |
| Free fixed list built from mapped selections | A reduction observes sparse item updates, invalidation, and revalidation. |
| Free bundle recombined from mapped reference fields | Independent field counts reflect partial field updates. |
| Switch outer dictionary before nested map selection | Selected values and event counts follow both sources. Updates to the inactive source alone produce no output; switching back samples its re-added nested item. |

## Findings

All ten new cases pass. No implementation defect or design gap was reproduced.
The cases exercise selected combinations, not the full product of reference
producers, consumers, and higher-level graphs. The selected patterns require no
unimplemented functionality, so none were excluded on that basis.

## Verification

The custom focused runner, including the new cases and existing reference
consumer tests, passes **45 cases and 45 assertions**. Structured coverage
validation classifies 45 tests with no errors. `git diff --check` and the
120-column check pass. The full HG test suite was not run.

The new source is registered in `hg_library_tests` and this report is linked
from the development index.

Combined integration verification includes this sweep, the
[service queue sweep](hgraph-service-queue-sweep-2026-11-03.md), the
[reduce composition sweep](hgraph-reduce-composition-sweep-2026-11-03.md), and
existing tests in the affected areas. These groups contain distinct cases:

| Focused group | Cases | Assertions | Result |
| --- | ---: | ---: | --- |
| Engine: references, collections, map, mesh, reduce, switch, nested graphs | 458 | 1,213 | Pass |
| Library: references, streams, merge, collections, reductions | 281 | 531 | Pass |
| Services, queues, and feedback | 161 | 651 | Pass |
| Total | 900 | 2,395 | Pass |

All 22 new cases have valid structured coverage tags. The complete library
and service groups also pass tag validation. These focused runs do not
replace a full-suite run.

## Scope limits

This batch checks selected nested-map, mesh, switch, collection, and bundle
compositions. It does not execute the complete upstream source-by-consumer
product or establish parity for combinations outside these supported APIs.
