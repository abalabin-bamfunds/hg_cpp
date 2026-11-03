# hgraph timed lifecycle sweep, 2026-11-03

This sweep checks scheduled operator work across map branches, switch branches,
and nested graph stop and restart. It adds tests and this report only. The HG
baseline is `e0156e84f`.

## Sources and existing coverage

The source snapshots are hgraph 0.5.42 at commit
`c34dba1db8a1c873ee303796db4c8fc48c8cc5be` and the 0.8 development snapshot
at commit `d36be500ec8b6fda77ff077566f891ffcf5c84a8`. Source tests were read
from these committed revisions.

Existing 0.5 coverage:

- `_wiring/test_switch.py::test_stop_start` and `test_nested_switch` check
  switch branch stop lifecycle, delayed work stopped with a branch, and a
  resample branch inside a switch. The nested test uses a timed generator
  inside map and a switch.
- `_operators/test_const.py::test_delayed_const` and `test_delayed`, plus
  `_operators/test_stream_operators.py::test_throttle_tsd`, cover delayed
  values, fallback, and collection throttling.

Existing 0.8 coverage:

- `test_scheduler_pending_events.py::test_many_pending_alarms_fire_in_order`
  checks timer ordering and nested scheduling.
- `test_nested_reference_context_retick.py` checks reference-backed context
  propagation and branch replacement without unnecessary re-ticks.

HG's timed operators follow the [library contract](../design/library.md#timed-operators).
Inner graph scheduling follows the [nested graph lifecycle](../design/nested-graphs.md#lifecycle).
A stopped graph discards its scheduled work. A new graph instance samples its
inputs when it starts.

## Added coverage

The cases are in
[`timed_lifecycle_patterns_test.cpp`](../../../tests/library/timed_lifecycle_patterns_test.cpp).

- Per-key delayed scalar branches: removing a map key cancels its pending event,
  a sibling continues, and a recreated key starts a new delay interval.
- Per-key throttling: mapped keys maintain independent coalescing state; removal
  and recreation do not reset the sibling's pending event.
- Delayed nested collections: deltas retain their order; removing an outer key
  cancels its pending inner changes while a sibling continues.
- Delayed switch branches in map: changing one key's branch cancels its pending
  event while the sibling continues.
- Nested delay stop and restart: stopping discards pending work; restarting
  publishes only events from the new graph instance.

The cases use `run_graph` simulation ticks and assert captured output deltas,
including quiet cycles and timer output after the final input where applicable.

## Findings and verification

All five added cases pass against the refreshed runtime. The mapped delay and
throttle traces include the lifecycle transitions and publication times
specified by the timed operator contract. The mapped switch trace confirms
that replacing one key's branch leaves the sibling branch active. The nested
collection case captures `TSD<i64, TSD<i64, TS<i64>>>` directly with
`copy_tick`, so its expected trace records sparse inner deltas. Removing an
outer key cancels that branch's pending changes while the sibling continues.
Restarting the nested graph discards events scheduled by its previous
instance.

The focused test runner passed all five cases. The full HG suite was not run
for this batch.

## Scope limits

These cases cover selected delay and throttle compositions. They do not establish
the full product of timed operators, nested graph owners, reference types, and
collection shapes. No production code was changed.
