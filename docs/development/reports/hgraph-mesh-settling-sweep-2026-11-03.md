# hgraph mesh settling sweep, 2026-11-03

This batch adds three tests for dynamic mesh dependencies and makes no
production changes. The HG baseline is `e0156e84f`. The patterns are from the
hgraph 0.8 snapshot
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8).
The 0.5.42 snapshot
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be)
was also reviewed; these dynamic settling patterns occur in the 0.8 tests.

## Patterns and coverage

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

All three cases pass against native libraries built from `e0156e84f`. The
focused command was `timeout 20s build/hgraph-mesh-settling-tests
'[feature:mesh]'`; Catch2 reported three assertions passing in three cases.
The test executable compiled with `cmake --build build/mesh-runner --target
hgraph-mesh-settling-tests -j2`.

Classification: these are test coverage gaps, not runtime defects. No failure
was reproduced at the sweep baseline.

## Scope limits

The tests assert the output of the retargeted branch, unchanged dependent, and
top-level branch that uses the switch-expanded dependency tree. They do not
assert the complete mesh output map or measure performance.
