# hgraph mesh settling sweep, 2026-11-03

This mesh sweep adds three tests for dynamic dependencies. The initial HG
baseline is `e0156e84f`. The patterns are from the
hgraph 0.8 snapshot
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`][hgraph-08].
The 0.5.42 snapshot
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`][hgraph-05]
was also reviewed; these dynamic settling patterns occur in the 0.8 tests.

## Patterns and coverage

The source cases in `tests/cpp/test_mesh.cpp` cover retargeting without a pause
loop, re-ticking a quiescent dependency, and waiting while a dependency expands.
The staged switch case also follows
`test_mesh_waits_for_a_paused_dependency_before_settling_its_dependent` in
`python/tests/test_mesh_dependency_settling.py`.

- **Retarget to a quiescent branch.** HGraph tests that a dependent can move
  from an on-demand branch to an existing branch that has already settled.
  Existing BG coverage follows a changing dependency key alongside branch
  updates, but not this case.
- **Re-tick an unchanged link.** HGraph checks that a dependency branch can be
  re-read without emitting a new dependent output. Existing BG coverage checks
  that a clean dependency is not reevaluated after a different branch updates.
- **Expand dependencies through a switch.** HGraph checks that a dependent
  settles after a switch selects a branch that adds two dependencies. Existing
  BG coverage tests switch and mesh composition, but not this settling path.

## Added tests

[`mesh_settling_patterns_test.cpp`](../../../tests/evaluation/mesh_settling_patterns_test.cpp)
checks the three paths above. It captures the selected scalar branch output so
`run_graph` does not need to capture nested mesh references.

## Findings and verification

All three cases pass against native libraries built from `e0156e84f`.

Classification: these are test coverage gaps, not runtime defects. No failure
was reproduced at the sweep baseline.

## Integration verification

The review lane was refreshed to main at `be009f7b3` and checked with the
dispatch collection compatibility fix. Both native libraries were rebuilt in
Release mode. The three focused runners include the new sources and existing
tests for the affected areas:

| Group | Added cases | Passing cases | Passing assertions |
| --- | ---: | ---: | ---: |
| Mesh, context, nested graph owner | 3 | 61 | 78 |
| [Timed lifecycle](hgraph-timed-lifecycle-sweep-2026-11-03.md), timed operators, scheduler | 5 | 114 | 494 |
| [Dispatch composition](hgraph-dispatch-composition-sweep-2026-11-03.md), casts, selection | 7 | 42 | 74 |
| Total | 15 | 217 | 646 |

The sources are registered in the normal `hg_tests` and `hg_library_tests`
targets. Structured coverage tags validate for all three focused runners with
no diagnostics. Whitespace, line widths, and report links were checked. The
full HG suite was not run for this batch.

## Scope limits

The tests assert the output of the retargeted branch, unchanged dependent, and
top-level branch that uses the switch-expanded dependency tree. They do not
assert the complete mesh output map or measure performance.

[hgraph-08]: https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8
[hgraph-05]: https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be
