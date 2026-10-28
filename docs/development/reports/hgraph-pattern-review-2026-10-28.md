# hgraph graph-pattern review

This review uses hgraph tests to select additional BG behavior tests. It focuses
on references across nested graph boundaries, free bundles and fixed lists, and
compositions of map, switch, mesh, and reduce. Implementation changes require a
separate decision after review.

## Source revisions

The upstream references were fetched from
[`hhenson/hgraph`](https://github.com/hhenson/hgraph) on 2026-10-28.

| Source | Revision |
| --- | --- |
| BG baseline | `11ba59a4` |
| hgraph `release/0.5`, latest tag `v_0.5.42` | [`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be) |
| hgraph `main`, latest tag `0.8.37` | [`b9305bb23f8e54337cd1941447b940e5fd43ec59`](https://github.com/hhenson/hgraph/tree/b9305bb23f8e54337cd1941447b940e5fd43ec59) |

The 0.5 release branch still points to 0.5.42. The 0.8 snapshot includes changes
through 2026-10-28. The earlier
[functionality survey](hgraph-functionality-survey-2026-10-21.md) uses different
BG and 0.8 revisions and does not establish the results of this review.

## Classification

| Status | Evidence required |
| --- | --- |
| Added, passing | A new BG test runs and asserts the behavior. |
| Already covered | An identified existing BG test asserts the same requirement. Source inspection alone is not a new verification run. |
| Reproduced failure | A supported graph produces a trace or failure that contradicts an identified BG contract. Keep its reproduction and expected result. |
| Semantic difference | The source expectation differs from BG's documented contract. State both expectations. |
| Unsupported, skipped | The source pattern requires an operation or declaration outside BG's supported API. State that requirement. |
| Harness limitation | The graph behavior cannot be established through the selected capture or input mechanism. |
| Unverified | The pattern has not been run or its intended behavior remains unresolved. |

The source tests select requirements; they are not an automatic oracle for BG.
In particular, the three versions have different reduction rules for empty and
singleton collections. BG follows its [reduce contract](../design/reduce.md).

Each lane report maps source tests to BG tests or records why they were not
ported. This source traceability is separate from the structured Catch2 tags,
which remain the [coverage source](../testing.md#coverage-model).

## Reports

- [References and nested input binding](hgraph-reference-pattern-findings-2026-10-28.md).
- [Free bundles and fixed lists](hgraph-free-bundle-pattern-findings-2026-10-28.md).
- [Nested graph compositions](hgraph-nested-pattern-findings-2026-10-28.md).

## Verification boundaries

The existing Release build at the BG baseline passed before new tests were
added: **2,230 engine cases** and **443 library cases**. This was the clean
`graph-output-inference` lane at `11ba59a4`, using its existing build artifacts.

The review ports selected patterns to BG and runs BG tests. It does not claim
to rerun either complete hgraph suite, establish Python authoring parity from a
C++ test, or measure runtime performance. Source parameterizations and new BG
test cases are counted separately.

Failing reproductions stay outside the normal test executable until the owner
decides which behaviors to address. They retain their required assertions and
have explicit commands in the relevant report. Passing tests stay in the
ordinary test suite.

## Scope exclusions

These source patterns require functionality outside the current BG contract.
They were inspected to identify the requirement and were not executed as BG
reproductions.

| Source test | Required functionality | Disposition |
| --- | --- | --- |
| 0.5 `_wiring/test_map.py::test_tsl_map_wiring`, `test_tsl_map_wiring_no_key`; 0.5/0.8 `_wiring/test_reduce.py::test_tsl_reduce*` | Higher-order map/reduce over fixed TSL | BG's [map](../design/map.md) and [reduce](../design/reduce.md) operate on TSD. Fixed TSL storage and free TSL construction have separate tests. |
| 0.5 `_wiring/test_reduce.py::test_reduce_map`; 0.8 `test_reduce_zero_semantics.py::test_reduce_mapped_bundle_collection_projection_dereferences_key_set_source` | A collection-valued reducer output | BG reduce requires scalar-valued time series output. |
| 0.5 `_wiring/test_switch.py::test_reduce_from_switch` | A bundle-valued reducer output | The surrounding map/switch/free-bundle operations are supported; the reducer output is outside the reduce contract. |
| 0.5 `_wiring/test_reduce.py::test_reduce_tuple`; 0.8 `test_reduce_zero_semantics.py::test_ordered_reduce_retains_delayed_source_schedules` | Ordered, non-associative reduction and a time series zero | BG reduce requires an associative and commutative operation and accepts a scalar default. |
| 0.8 `ported/_wiring/test_context.py::test_context_crosses_stacked_try_except_boundaries` | A graph boundary that captures exceptions as output | BG [exception handling](../design/evaluation.md#exceptions) stops the graph and propagates the exception. No `try_except` operation was found in the public API. |
| 0.8 `test_distributed_map.py::test_state_churn_and_quiet_cycles`, `test_default_hosting_uses_a_separate_process` | Worker processes and distributed graph execution | The public BG map API runs local inner graphs. Local map lifecycle coverage does not establish distributed execution. |

Checkpoint recovery, external adapters, rolling-window operators, and the full
operator-by-reference Cartesian product are outside this round. Services,
feedback, and Python-specific conversion cases that were not explicitly mapped
in a lane report remain unverified by this review. Existing functionality is
not classified as unsupported solely because it was not selected here.
