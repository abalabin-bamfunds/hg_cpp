# hgraph structured outputs and mapped timers sweep, 2026-11-04

This sweep checks structured mesh outputs, dispatch collection validity, and
mapped timed sources against HG baseline `16d1513eb`. It adds tests and records
findings for review. Production code is unchanged.

## Source patterns

The reviewed hgraph snapshots are 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`][hgraph-05] and 0.8 development at
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`][hgraph-08]. Source inspection uses
committed Git objects.

- Mesh: 0.8 `tests/cpp/test_mesh.cpp`, especially stable field endpoints from
  structured `mesh_ref` outputs and later updates from forwarded child outputs.
- Dispatch: 0.8 `tests/cpp/test_dispatch.cpp` and
  `python/tests/test_dispatch_covariant_output.py`, combined with collection
  validity and reference-selection patterns.
- Timers: 0.5.42 `hgraph_unit_tests/_wiring/test_switch.py::test_nested_switch`
  maps independently started timed sources over dynamic keys. The 0.8
  `python/tests/ported/_operators/test_stream_operators.py` supplies schedule
  and sampling patterns.

The added cases combine supported operations from these patterns. They are not
an execution of the complete upstream suites.

## Added coverage

[`mesh_structured_patterns_test.cpp`](../../../tests/evaluation/mesh_structured_patterns_test.cpp)
adds six cases:

- Free bundle and fixed-list outputs publish only the changed field or item.
- A requested branch creates another branch on demand and reads its bundle
  fields through a switch. Both outputs receive later source updates.
- A branch reads another branch's list items through references.
- Removing and recreating bundle and list branches emits complete new values
  while another branch remains present.

[`dispatch_validity_patterns_test.cpp`](../../../tests/evaluation/dispatch_validity_patterns_test.cpp)
adds four cases:

- Type changes that return the same scalar target do not create value ticks.
- Free and native bundle and fixed-list outputs preserve partial validity across
  updates, invalidation, revalidation, and concrete input type changes.
- Direct and copied dictionary outputs preserve invalid items, removals, root
  invalidation, and revalidation across branch replacement.

[`mapped_timed_sources_test.cpp`](../../../tests/library/mapped_timed_sources_test.cpp)
adds six cases:

- Delayed and immediate finite schedules start independently for staggered keys.
- Removing a key on its deadline cancels its tick. Recreating it starts a fresh
  schedule while the other key continues.
- Resampling uses each branch's start time and reads input updates that coincide
  with its timer.
- Removing a branch on a resampling deadline prevents its pending sample.
  Recreating the branch restarts its clock while its sibling continues.

## Classification

The tested paths fill coverage gaps. No production defect was reproduced.
Collection branch replacement samples all valid fields or items when the
resolved target storage changes, including held values. Returning a scalar
through a new reference path to the same target does not create a value tick.
These behaviors follow the [reference sampling contract](../design/time-series-references.md#input-sampling).

## Verification

All 16 added cases pass. The tests were compiled in Release mode and linked
against native libraries matching the baseline implementation. The focused
runners also include existing tests for the affected areas:

| Group | Added cases | Passing cases | Passing assertions |
| --- | ---: | ---: | ---: |
| Mesh, context, nested graph owner | 6 | 67 | 84 |
| Dispatch, casts, selection | 4 | 46 | 78 |
| Timed operators, debounce, buffering, emit, scheduler | 6 | 120 | 500 |
| Total | 16 | 233 | 662 |

The new sources are registered in `hg_tests` and `hg_library_tests`. Structured
coverage tags validate for all three focused runners. Whitespace, new-source
line widths, and report links pass verification. The full HG suite was not run.

## Scope and adaptations

Mesh and map outputs contain references. A normal `copy_tick` consumer copies
their observable deltas to native output storage for capture. This follows the
existing [capture restriction](../testing.md#test-layout) on reference-valued
collection items.

Schedules use finite tick limits. Resampling uses bounded simulation runs.
The mapped timer cases combine upstream timed-source and sampling patterns;
they do not port the complete generator-and-switch trace. Existing
[`timed_lifecycle_patterns_test.cpp`](../../../tests/library/timed_lifecycle_patterns_test.cpp)
retains mapped delay, throttle, and switch lifecycle coverage.

Upstream `test_schedule_ts` and `test_schedule_ts_with_start` are excluded API
gaps. HG's [`schedule` overload](../../../src/library/timed.cpp) accepts a scalar
period, an initial-delay flag, and a tick limit. It has no time-series period or
absolute-start argument. No runtime failure is claimed for these unsupported
calls.

Wall-clock timers, large pending-alarm counts, Python reflection, and Python
overload recreation are outside this batch.

[hgraph-05]: https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be
[hgraph-08]: https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8
