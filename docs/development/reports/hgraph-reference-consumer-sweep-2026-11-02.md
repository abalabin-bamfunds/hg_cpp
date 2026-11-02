# hgraph reference-consumer sweep, 2026-11-02

This batch checks metadata and stateful consumers of references, free fixed
collections across nested graph boundaries, and references retained across
branch removal and recreation. It extends the
[preceding sweep](hgraph-reference-consumer-sweep-2026-10-30.md).
The initial baseline was `84d756fe`. The batch is now rebased onto HG
`362c103dd`. It changes tests and documentation only.

## Sources and interpretation

The source snapshots are hgraph 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be)
and the hgraph development snapshot used for the 0.8 patterns at
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8).
The selected files in the latter local checkout have no changes from that
commit. Other local changes in that checkout are outside this review.

| Source | Pattern |
| --- | --- |
| 0.8 `python/tests/test_ref_consumer_sweep.py::test_ref_source_matches_plain` | Feed metadata and stateful consumers through dictionary selection, fixed-list selection, mapped items, conditional references, and switch outputs. |
| 0.5 `hgraph_unit_tests/_wiring/test_tsb_wiring.py::test_free_tsb_signal` and `test_free_tsb_ref_signal`; 0.8 ported counterparts | Observe independent free-bundle fields, partial validity, and changes across switch branches. |
| 0.5 `hgraph_unit_tests/_wiring/test_tsl_wiring.py::test_fixed_tsl_non_peered_input` and `test_tsl_ref_flipping`; 0.8 ported counterparts | Construct and select free fixed lists, then consume their item state across reference changes. |
| 0.5 `hgraph_unit_tests/_wiring/test_map.py::test_map_reference_cleanup` and `test_map_reference_cleanup_2` | Keep downstream consumers alive while upstream map branches disappear and return. |
| 0.5 `hgraph_unit_tests/_wiring/test_reference_observer_lifetime.py::test_cascaded_switch_in_map_ticking_into_take`; 0.8 ported counterpart | Compose nested reference-producing graphs with stateful downstream consumers. |

The upstream product sweep supplies candidate compositions. Expected results
follow HG's contracts. A dereferencing binding retains the new reference handle
but samples only when the resolved target changes. Equal values in distinct
target storage still permit a sample. Explicit REF inputs receive ordinary
reference ticks. See the
[reference contract](../design/time-series-references.md#subscriptions).

The tests assert both values and quiet cycles. Fixed-collection tests observe
individual items or fields. Retained selections ignore unrelated-key updates;
tests of independent per-key state observe both keys. Returned
collections with reference leaves are consumed through ordinary graph nodes,
as required by the [capture limitation](../testing.md).

## Added coverage

The 35 new cases are in
[`reference_consumer_test.cpp`](../../../tests/library/reference_consumer_test.cpp).
Each case shows its graph, input deltas, and expected output.

| Area | Assertions |
| --- | --- |
| Selected dictionary items and identity-map items | `valid`, `modified`, `lag`, `take`, `drop`, and `dedup`; selected-item updates and quiet sibling updates |
| Target changes | Equal values in distinct targets publish samples; same-target switch branch replacement leaves `count` and `lag` unchanged |
| Selected nested dictionaries | `keys_`, `len_`, and `is_empty` through direct selection and mapped references; empty values, membership changes, unrelated outer keys, and removal/recreation of the selected outer key |
| Contained values | Inner scalar selection and sparse fixed-list item updates through mapped references |
| Branch lifetime | Downstream `count` follows a retained map or mesh selection, resets after removal, and resumes after recreation |
| Free collections | Field selection and partial validity across switch branches; free list and bundle construction across nested graph boundaries |

## Findings

All 35 new cases pass. This batch confirms no new implementation defect or
design gap in the tested compositions. It adds no hidden reproductions and
changes no production code.

The two-key map/switch cases assert separate `count` and `lag` outputs for
asymmetric updates, removal/recreation of one key, and subsequent source
switching. The free-list and free-bundle switch cases assert exact item deltas
when branch replacement coincides with source updates. They also assert item
invalidation and revalidation. The mapped free-bundle case checks independent
field updates and ignores changes in an unrelated branch.

## Verification

The focused binaries use GCC Release flags (`-O3 -DNDEBUG`) and matching native
libraries. After rebasing onto `362c103dd`, the test includes and namespace were
updated for the `hg` rename. The native libraries and both focused binaries were
rebuilt. Both the implementation agent and the main agent ran the complete
focused binaries and confirmed the same counts:

| Runner | Test cases | Assertions | Result |
| --- | ---: | ---: | --- |
| Focused library | 268 | 517 | Pass |
| Focused engine | 455 | 1,210 | Pass |
| Combined | 723 | 1,727 | Pass |

The library runner includes `stream_test.cpp`, `collection_test.cpp`,
`merge_test.cpp`, `tsd_transform_test.cpp`, `tick_selection_test.cpp`, and the
new `reference_consumer_test.cpp`. The engine runner retains the preceding
sweep's map, mesh, reduce, switch, nested-graph, context, collection,
reference, valid-item, and mesh-wiring cases. The full HG test suite was not run.

Run the refreshed binaries from the review lane:

```sh
build/integration/hgraph-pattern-hg-library-tests
build/integration/hgraph-pattern-hg-engine-tests
```

Library coverage tags validate with no errors, including all new cases.
Engine tag validation reports the same five baseline diagnostics: one missing
feature tag, one unknown `invalidation` dimension, two unknown `lifetime`
dimensions, and one undocumented `definition:graph` value. No engine test
changed in this batch. Whitespace and documentation link checks pass.

## Scope limits

This batch does not execute the complete upstream source-by-consumer product.
Services and feedback remain a separate review. Unsupported higher-order
map/reduce over fixed lists, collection fallback, and dictionary-wide attribute
projection remain excluded. Supported explicit field projection is reviewed
through normal map graphs.
