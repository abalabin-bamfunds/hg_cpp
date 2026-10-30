# hgraph pattern sweep, 2026-10-30

This sweep adds behavior tests for higher-level collection and graph patterns
from hgraph 0.5 and 0.8. It checks reference routing through maps and nested
collections, empty collection states, and updates after membership changes. It
does not change production code.

The BG baseline is `61574125750b0f048432ebb7dd8d10d1f507491d`. The upstream
snapshots are hgraph 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be)
and hgraph's current 0.8 development branch at
[`7e45748c966e8cf6c3cb2fbf57adfbc259302040`](https://github.com/hhenson/hgraph/tree/7e45748c966e8cf6c3cb2fbf57adfbc259302040).
Earlier source mappings remain in the [review summary](hgraph-pattern-review-2026-10-28.md),
[reference report](hgraph-reference-pattern-findings-2026-10-28.md),
[free-structure report](hgraph-free-bundle-pattern-findings-2026-10-28.md),
and [nested-composition report](hgraph-nested-pattern-findings-2026-10-28.md).
The [latest recheck](hgraph-pattern-recheck-2026-10-30.md) records the status of
the earlier findings. This sweep adds six cases; it does not rerun those reports'
historical failure audits.

## New BG cases

All six cases pass. Each case checks a supported BG composition derived from
the named upstream test.

| BG test | Upstream source | Coverage |
| --- | --- | --- |
| `dedup publishes an initially empty dictionary before its first item` in [`stream_test.cpp`](../../../tests/library/stream_test.cpp) | 0.5 `_operators/test_stream_operators.py::test_drop_dups_tsd_preserves_initial_empty_value` | Empty, first item, removal, and re-addition states |
| `dedup publishes an empty dictionary child in a bundle` in [`stream_test.cpp`](../../../tests/library/stream_test.cpp) | 0.5 `_operators/test_stream_operators.py::test_drop_dups_tsb_preserves_initial_empty_tsd_child` | A free bundle containing an initially empty dictionary child |
| `dictionary selection follows references returned by an identity map` in [`collection_test.cpp`](../../../tests/library/collection_test.cpp) | 0.5 and 0.8 `_operators/test_tsd_operators.py::test_tsd_get_items_refs` | Direct and mapped selection across value updates, selection changes, and source removal |
| `merge combines nested dictionaries and forwards inner changes` in [`merge_test.cpp`](../../../tests/library/merge_test.cpp) | 0.5 and 0.8 `_operators/test_tsd_operators.py::test_merge_nested_tsd` | Overlapping inner keys, removal, and update |
| `partitioned dictionaries reduce group totals across moves and removals` in [`tsd_transform_test.cpp`](../../../tests/library/tsd_transform_test.cpp) | 0.5 and 0.8 `_operators/test_tsd_operators.py::test_partition_with_reduce` | Exact group totals as values and membership change; empty group removal and re-creation |
| `nested map projection preserves the final inner key removal` in [`tsd_transform_test.cpp`](../../../tests/library/tsd_transform_test.cpp) | 0.8 `test_tsd_removal_semantics.py::test_nested_map_projection_preserves_final_key_removal` | Bundle field projection through nested maps and key collapse; the outer key remains after its final inner key is removed |

Source paths are relative to `hgraph_unit_tests/` for 0.5 and
`python/tests/ported/` for the 0.8 operator tests. The 0.8 removal test is under
`python/tests/`.

The partition case uses integer source keys and explicit string group mappings
instead of upstream's compound-key projection. The nested projection case uses
a native nested-TSD input instead of Python scalar conversion, `default`, and
`filter_by`. It covers the upstream map projection variant and asserts exact
collapsed deltas; it does not inspect every removal accessor.

The merge and collapse tests pass their results through `copy_tick` nodes. The
graph-run capture API does not support nested time-series reference outputs;
materialization lets these tests verify the resulting collection deltas through
the normal downstream graph path.

## Classification notes

No new graph-runtime defect was confirmed.

BG `TSD` `getitem` keeps a selected key when the source key is removed and
invalidates its reference. Upstream's selection tests remove that result key.
The new direct and identity-map traces both end with `{2: INVALID}`, as required
by BG's [collection-selection contract](../design/library.md#collection-selection).

Nested merge selects the leftmost valid changed input for an overlapping key.
The new test asserts this precedence. Partition uses BG's default
`remove_empty=true`, so removing a group's last member removes that group.

## Remaining scope

Three-level `test_collapse_more_keys_tsd`, `test_uncollapse_more_keys_tsd`, and
its keep-empty variant require more than BG's supported
[two key levels](../../user/reference/library-collections.md#dictionary-transforms).
They remain excluded.

This round does not cover compound-key projection, switching the selected
source dictionary (`test_tsd_get_items_change_tsd`), the attribute projection
variant, Python conversion behavior, or the full REF-consumer parameterizations.
Services and feedback patterns also remain outside this round. The six passing
cases do not establish coverage of the full upstream suites.

## Verification

The focused library runner passed **221 test cases and 469 assertions**. It
includes all cases from `stream_test.cpp`, `collection_test.cpp`,
`merge_test.cpp`, `tsd_transform_test.cpp`, and `tick_selection_test.cpp`.
It was built with GCC Release flags (`-O3 -DNDEBUG`) against the matching BG
runtime. The complete BG test suite was not run for this tests-and-report change.
