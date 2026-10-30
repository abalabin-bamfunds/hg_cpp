# Map reference retarget investigation

This report preserves the original investigation of missing-item updates in
`map(identity, selected)` when `selected` changes between dictionaries, and
records verification against the latest code. This lane changed no production
code.

The original reproduction was checked against BG revision `cab5c342` using the
Release runtime. The regression test is now an ordinary map test at
[`map_test.cpp`](../../../tests/evaluation/map_test.cpp#L957).

## Historical trigger and stage comparison (`cab5c342`)

The minimal trigger has three parts: a selector changes the selected
`TSD<i64, TS<i64>>` reference; `map(identity, selected)` returns each mapped
item by reference; and a value `TSD` consumer copies the mapped output by tick.
A direct selector with value capture passes, so the nested graph in the original
regression is not required. At tick 1, the selected dictionary is
`{1: 1, 2: 2}`. At tick 2, selection changes to a dictionary containing
`{1: 11, 2: 20}`, where key 2 has not itself updated on that tick. Selection
changes back at tick 3.

| Graph output path | Observed ticks | Result |
| --- | --- | --- |
| Direct selector, capture dictionary | `{1:1,2:2}`, `{1:11,2:20}`, `{1:3,2:2}`, `{1:4}` | Passes |
| Nested selector, capture dictionary | Same as direct capture | Passes |
| Direct selector, reference-return map | `{1:1,2:2}`, `{1:11}`, `{1:3}`, `{1:4}` | Omits key 2 after each retarget |
| Nested selector, reference-return map | Same as direct reference-return map | Omits key 2 after each retarget |
| Nested selector, materializing map | Same as direct capture | Passes |
| Nested selector, scalar-output map with selector input | Same as direct capture | Passes; this changes the mapped output type, so it does not independently test scheduling |

The direct-selector result removes nested graph boundaries from the necessary
trigger. The materializing result narrows the failure to the reference-return
output path. The scalar-output case is only a comparison; its changed output
type prevents conclusions about whether scheduling alone matters.

## Historical metadata observation (`cab5c342`)

A temporary observer read key 2 from the reference-return map output before
`copy_tick`. It recorded the value, dictionary and item modification flags, and
dictionary and item update times:

| Tick | Value | Dictionary modified | `item.modified()` | `item_modified(2)` | Dictionary time | Item time |
| --- | ---: | --- | --- | --- | ---: | ---: |
| 1 | 2 | true | true | false | 1 | 1 |
| 2 | 20 | true | false | true | 2 | 1 |
| 3 | 2 | true | false | true | 3 | 1 |
| 4 | 2 | true | false | false | 4 | 1 |

Times are the microsecond evaluation timestamps produced by this run. At ticks
2 and 3, key 2 resolves to the newly selected value and the dictionary reports
the key modified. The scalar item still has its tick-1 timestamp and reports
unmodified. The value and reference routing are current; the item-level change
metadata is stale for the retarget.

## Historical source-path analysis (`cab5c342`)

`MapRuntimeState::update_entry_output` writes each mapped result as a reference
item in `TSD<SCALAR, REF<TS_T>>` ([`map_node.cpp`](../../../src/map_node.cpp)).
When a consumer requests the dereferenced dictionary representation,
`DereferencedTimeSeriesDictionaryRepresentation::bind_item` changes the item
reference and calls `sample_switch`
([`tsd_dereferenced_representation.cpp`](../../../src/time_series/tsd/tsd_dereferenced_representation.cpp)).
The representation records the dictionary item as modified while processing
the source dictionary change. Its `item()` method then returns the resolved
scalar target's own last-updated state.

Scalar sampling uses the reference's binding timestamp. For an ordinary node
input, `Node::binding_data` selects the later of the reference and target states
when the target is valid
([`node.hpp`](../../../include/bg/graph/node.hpp)).
`SampledReferenceChange::create` returning no collection sample for scalar `TS`
is expected; a scalar does not require collection transition data.

The missing operation is in the dictionary representation's `item()` method:
it returns the target state without comparing the source `REF` item's state.
The reference's binding timestamp therefore does not reach the value item.
`In<TSD>::make_item` derives `modified()` from that returned timestamp
([`tsd_input.hpp`](../../../include/bg/time_series/tsd/tsd_input.hpp)).
The dictionary can report key 2 as modified while its scalar view reports the
old target time and `modified() == false`. This is a missing reference-time
selection in dictionary item access, not a missing scalar sample object.

The copy path exposes the mismatch. `Out<TSD>::copy_tick` iterates dictionary
items it sees as modified and calls the item output's `copy_tick`
([`tsd_output.hpp`](../../../include/bg/time_series/tsd/tsd_output.hpp)).
`Out<TS>::copy_tick` skips an item unless it was modified at the current
evaluation time ([`ts_output.hpp`](../../../include/bg/time_series/ts/ts_output.hpp)).
For the retargeted scalar, that guard rejects the current value because its
timestamp is still tick 1 and `item.modified()` is false. This matches the
missing key-2 values in the captured output.

## Historical verification (`cab5c342`)

From the main review lane, the freshly compiled map translation unit passed
with **30 cases and 42 assertions**. Running the hidden regression filter
against it gives one failing assertion with the missing key-2 updates:

```sh
LD_LIBRARY_PATH=build/verification/current-runtime build/verification/map-retarget-latest
LD_LIBRARY_PATH=build/verification/current-runtime build/verification/map-retarget-latest \
  'repro: map retargets a selected dictionary reference across a nested graph'
```

The runs are recorded in `build/verification/2026-10-29-map-ordinary.log` and
`build/verification/2026-10-29-retarget-latest.log` in the main review lane.

## Latest-code follow-up (`c8b33979`)

The latest source computes a dereferenced dictionary item's observed state with
`bindings_.references[id].observed_state(base_item.last_updated, referenced_item)`
in [`tsd_dereferenced_representation.cpp`](../../../src/time_series/tsd/tsd_dereferenced_representation.cpp#L224).
This combines the selected reference's transition state with the target item's
state, so a retarget remains observable when the target item itself did not
update. The promoted test expects the captured output deltas
`{1:1,2:2}`, `{1:11,2:20}`, `{1:3,2:2}`, and `{1:4}`. The final delta updates
key 1; it does not assert removal of key 2.

The map translation unit passed with **39 cases and 53 assertions**. The
promoted test passed independently with **1 case and 1 assertion**. The fresh
executable linked only the freshly compiled `map_test.cpp` object, Catch2, the
latest Release `libbg`, and `fmt`; `ldd` confirmed it loaded
`build/current-release/libbg.so.0`. Reproduce these runs with:

```sh
build/verification/map-retarget-post-merge
build/verification/map-retarget-post-merge \
  'map retargets a selected dictionary reference across a nested graph'
```

The outputs are recorded in `build/verification/2026-10-29-map-promoted-latest.log`,
`build/verification/2026-10-29-retarget-promoted-latest.log`, and
`build/verification/2026-10-29-promoted-runtime.log`.

## Scope and limits

The `cab5c342` metadata violated the documented input-sampling contract:
[`time-series-references.md`](../design/time-series-references.md#input-sampling)
requires the binding time to be visible as the sampled input's last-updated
time. The latest-code follow-up confirms the promoted case passes after the
dereferenced `TSD` path incorporates the selected reference's observed state.
This investigation does not assess nested bundle, set, or free-bundle targets.
