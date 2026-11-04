# hgraph Dict mapping, collection projection, and structured services sweep, 2026-11-04

This sweep checks higher-order scalar Dict mapping, collection fields projected
from mapped bundles, and structured service replies against HG `d334ad217`.
It adds tests and records findings for review. Production code is unchanged.

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

## Finding: collapsed items remain valid after their group is invalidated

Classification: runtime correctness defect. Both direct `collapse_keys` and
the mapped bundle projection retain two valid flattened items after the input's
nested TSD becomes invalid. Map and bundle projection do not introduce the
divergence.

| Cycle | Nested source group | Flattened keys | Valid flattened items | Guarded sum |
| ---: | --- | ---: | ---: | ---: |
| 0 | `{1: 10, 2: 20}` | 2 | 2 | 30 |
| 1 | invalid | 2 | 2 | 30 |
| 2 | `{1: 11, 2: 22}` | 2 | 2 | 33 |

The expected sum is `30, 0, 33`: [TSD invalidation](../design/time-series-validity.md#collections)
removes the collection's current value and reports its prior members as removed.
The probe checks each flattened item's validity before reading its value.
A separate observer confirms that the input group itself becomes invalid.

This minimized direct case reproduces the same result through the standard
optional sum, without a custom observer:

```cpp
const auto flattened_sum = [](wp<TSD<string, TSD<i64, TS<i64>>>> values) {
    return operators::sum_(opt(operators::collapse_keys(values)));
};

CHECK(run_graph(flattened_sum,
                {{{"A", {{1, 10}, {2, 20}}}},
                 {{"A", literal::INVALID}},
                 {{"A", {{1, 11}, {2, 22}}}}})
          .equals<i64>({30, 0, 33})); // Actual: 30, 30, 33.
```

The relevant boundary is
[`collapse_keys_tsd_impl::evaluate()`](../../../src/library/tsd_transform.cpp):
it processes inner removals for updated groups. The precise cause of the lost
invalidation remains unconfirmed. The failing reproducer is recorded here;
the four added projection tests cover passing paths. No production fix was made.

## Scope and adaptations

The service cases use HG service declarations. They do not add the upstream
runtime or erased service-adaptor interfaces, Python reflection, or erased
field-normalization APIs. The accepted recursive-service parity case is outside
this sweep.

Normal `copy_tick` nodes materialize reference-valued collection output for
capture, following the [capture restriction](../testing.md#test-layout).

## Verification

Focused runners use Release libraries built from `a2f3d723d`. The final branch
is rebased onto `d334ad217`; the intervening Python binding changes leave the
tested engine and standard library code unchanged.
All 16 added cases pass, alongside existing tests for the affected areas:

| Group | Added cases | Passing cases | Passing assertions |
| --- | ---: | ---: | ---: |
| Dict mapping, projections, map, nested, switch | 9 | 128 | 209 |
| Structured services and existing service behavior | 7 | 64 | 173 |
| Total | 16 | 192 | 382 |

Two existing switch tests used undocumented coverage tags. Their tags now use
the documented `definition:function` value and omit the unsupported `lifetime`
dimension. Their assertions are unchanged. The service runner's structured tags
and the Dict runner's structured tags validate. The new sources are registered
in `hg_library_tests`. Whitespace, new-source line widths, and report links pass
verification. The full HG suite was not run. The failing invalidation reproducer
above is separate from the passing regression runners.

[hgraph-05]: https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be
[hgraph-08]: https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8
