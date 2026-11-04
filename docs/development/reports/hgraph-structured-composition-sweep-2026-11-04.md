# hgraph Dict mapping, collection projection, and structured services sweep, 2026-11-04

This sweep checks higher-order scalar Dict mapping, collection fields projected
from mapped bundles, and structured service replies against HG `d334ad217`.
It adds tests and corrects nested TSD invalidation sampling.

## Source patterns

The reviewed hgraph snapshots are 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`][hgraph-05] and 0.8 development at
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`][hgraph-08]. Source inspection uses
committed Git objects.

- Dict mapping adapts map lifecycle and reference patterns to HG's supported
  `TS<Dict<K, V>>` map inputs. Sources include 0.5.42
  `hgraph_unit_tests/_wiring/test_map.py::test_map_input_rebind_to_nonpeer`
  and the 0.8 `NestedExplicitKeysMapSwitchG` case in `tests/cpp/test_map.cpp`.
- Collection projection uses 0.8 `tests/cpp/test_map.cpp`: late explicit keys,
  collection fields waiting for their source, and projected dictionary removals.
- Structured services adapt multi-field request/reply and reference-leaf patterns
  from 0.8 `tests/cpp/test_service_runtime.cpp`, plus subscription sampling from
  `python/tests/ported/_wiring/test_service.py`, to HG service declarations.

## Added coverage

[`map_dict_composition_patterns_test.cpp`](../../../tests/library/map_dict_composition_patterns_test.cpp)
adds five cases:

- A scalar Dict returned through a nested graph remains a mapped source.
- Equal snapshots preserve per-key state. Removing and recreating a key resets
  that key's state while a sibling remains alive.
- A nested switch retargets the Dict source and follows updates on the selected
  target.
- Selecting another Dict with an equal entry preserves that entry's branch
  state. Later target updates reach the existing branch.
- Mixed Dict and TSD inputs update branches for their union of keys.

[`map_projection_patterns_test.cpp`](../../../tests/library/map_projection_patterns_test.cpp)
adds four cases:

- A mapped bundle's TSD field forwards updates and nested item removals.
- A late explicit key samples both held members of a projected TSS field.
- A projected TSS field waits for its selected source, then samples its complete
  set.
- Two fields sharing the same TSD target both observe key addition and removal.

[`service_structured_patterns_test.cpp`](../../../tests/library/service_structured_patterns_test.cpp)
adds seven cases:

- Multi-field request/reply outputs preserve partial field updates.
- Reference services forward partial updates from native bundles and free
  bundles with explicit reference fields.
- Removing and recreating a mapped client leaves a sibling's reply intact.
- Switching away from a client cancels its pending delayed reply. Re-entry
  creates a fresh request while another client remains active.
- Independent clients update their own replies.
- A late subscription client samples both fields of an existing reply without
  requiring another implementation update.

## Semantics

A scalar Dict tick replaces the complete snapshot. Omitting a key removes it.
A TSD tick supplies item deltas, so omitted keys retain their current values.
Mixed-source expectations follow both rules. Equal Dict entries are deduplicated
by the [scalar Dict conversion](../design/map.md#arguments), including when
the source reference changes.

Mapped clients whose service implementation consumes their requests use
[`feedback_service_implementation`](../design/services.md#implementation-multiplicity)
to break the request/reply dependency cycle. Their replies arrive one cycle
after their requests. An invalid reply removes the key when the outer map uses
`__valid_items__ = true`.

## Resolved finding: nested TSD invalidation loses sampled removals

Classification: runtime correctness defect in the dereferenced TSD
representation. When a nested TSD invalidated, its sampling metadata used the
outer representation's older timestamp. The sampler therefore skipped the
child's current-cycle removals. Both direct `collapse_keys` and mapped bundle
projection retained valid flattened items for the invalid group.

[`notify_item_change()`](../../../src/time_series/tsd/tsd_dereferenced_representation.cpp)
now reconstructs the child's pre-cycle validity at the current sampling time.
The existing sampler then enumerates its removed members. The correction adds
no nodes or storage and leaves the ordinary update path unchanged.

[TSD invalidation](../design/time-series-validity.md#collections) removes the
collection's current value and reports its prior members as removed. This
minimal graph therefore emits `30, 0, 33`; before the correction it emitted
`30, 30, 33`:

```cpp
const auto flattened_sum = [](wp<TSD<string, TSD<i64, TS<i64>>>> values) {
    return operators::sum_(opt(operators::collapse_keys(values)));
};

CHECK(run_graph(flattened_sum,
                {{{"A", {{1, 10}, {2, 20}}}},
                 {{"A", literal::INVALID}},
                 {{"A", {{1, 11}, {2, 22}}}}})
          .equals<i64>({30, 0, 33}));
```

Regression coverage is in
[`tsd_transform_test.cpp`](../../../tests/library/tsd_transform_test.cpp).
Direct and mapped bundle-field paths check exact flattened removals and sums,
retain a sibling group, and revalidate with different inner keys. A separate
case verifies that invalidating a scalar leaf retains its physical key.

## Scope and adaptations

The service cases use HG service declarations. They do not add the upstream
runtime or erased service-adaptor interfaces, Python reflection, or erased
field-normalization APIs. The accepted recursive-service parity case is outside
this sweep.

Normal `copy_tick` nodes materialize reference-valued collection output for
capture, following the [capture restriction](../testing.md#test-layout).

## Verification

Focused runners use Release engine and standard library builds containing the
invalidation correction on `d334ad217`. All 18 added cases pass, alongside
existing tests for the affected areas:

| Group | Added cases | Passing cases | Passing assertions |
| --- | ---: | ---: | ---: |
| Dict mapping, projections, map, nested, switch | 9 | 128 | 209 |
| Structured services and existing service behavior | 7 | 64 | 173 |
| Nested invalidation and existing library/reference behavior | 2 | 283 | 536 |
| Total | 18 | 475 | 918 |

The broader Release runtime runner also passes 460 cases and 1,215 assertions.
The original standalone reproducer passes with `30, 0, 33`.

The full Debug C++ suite passes all 3,269 CTest tests, including both invalidation
regressions, declaration identity, and isolated logging tests. This run uses the
correction on `d334ad217`; the 16 composition cases above are checked by the
Release runners. The Debug build uses matching ICU 78.3 headers and libraries.

Two existing switch tests used undocumented coverage tags. Their tags now use
the documented `definition:function` value and omit the unsupported `lifetime`
dimension. Their assertions are unchanged. Structured tags validate for all
three runners. The new sources are registered in `hg_library_tests`.
Whitespace and report links pass verification.

The broader runtime runner's coverage-tag check reports two existing errors:
the preserving TSD test uses the unsupported `invalidation` dimension, and the
delayed-reference free-TSB test lacks a `feature` tag. Both tests pass; their
sources are unchanged by this correction.

[hgraph-05]: https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be
[hgraph-08]: https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8
