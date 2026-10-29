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
| BG initial baseline | `11ba59a4` |
| BG 2026-10-28 suite validation base | `8058a013` |
| BG 2026-10-29 reference investigation base | `cab5c342` |
| BG post-library-merge retarget validation base | `c8b33979` |
| hgraph `release/0.5`, latest tag `v_0.5.42` | [`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be) |
| hgraph `main`, latest tag `0.8.37` | [`b9305bb23f8e54337cd1941447b940e5fd43ec59`](https://github.com/hhenson/hgraph/tree/b9305bb23f8e54337cd1941447b940e5fd43ec59) |

The 0.5 release branch still points to 0.5.42. The 0.8 snapshot includes changes
through 2026-10-28. The earlier
[functionality survey](hgraph-functionality-survey-2026-10-21.md) uses different
BG and 0.8 revisions and does not establish the results of this review.

The root branch advanced during the review. The review lane was rebased onto
`8058a013`, which adds print and logging behavior. These commits do not change
map, switch, mesh, reduce, or reference runtime implementations.

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

## Results

The review adds **37 cases**. The original validation had **26 ordinary passing
cases** and **11 hidden reproductions**. After the library merge, the retargeting
case passes and runs in the ordinary suite, leaving **27 ordinary cases** and
**10 hidden reproductions**. The other ten reproductions have not been rerun
against the merge. A reproduction can expose a wiring failure, an output
mismatch, or an unresolved semantic difference; it is not automatically an
approved defect or implementation task.

| Finding | Cases | Classification |
| --- | --- | --- |
| Selected dictionary reference omits a held key on retargeting across a nested graph. | 1 | Resolved by the library merge; ordinary regression passes on `c8b33979`. |
| Switch rejects native/free TSB branches, rebuilt free TSB branches, or reference/owned TSD branches. | 3 | Wiring compatibility failures. The TSD failure blocks the cascaded map/switch/`take` pattern before evaluation. |
| Nested TSB/TSL REF pass-through attempts to create a reference targeting a reference. | 2 | Wiring failures against the documented reference-preserving nested boundary. |
| Mesh/switch/reduce compositions fail type resolution, including with typed callbacks. | 2 | Wiring failures; runtime behavior remains unverified. |
| Free TSB/TSL switch activations capture empty structural deltas while scalar consumers read held values. | 2 | Collection observation gap. Sampling and capture require separate investigation. |
| Reselecting a rebuilt free list emits held values where the source test expects silence. | 1 | Unresolved target-identity semantics. |

Passing additions exercise key removal/readdition, independent reference-backed
contexts, leaf updates and validity, nested free structures, pass-through
selection identity, and map/switch/mesh/reduce combinations. The detailed
reports identify exact ports, partial coverage, and new BG combinations.

The [retargeting investigation](reference-retarget-investigation-2026-10-29.md)
preserves the failure on BG `cab5c342` and records the passing result on
`c8b33979`. Dereferenced dictionary item access now uses the shared
`observed_state()` operation to include the reference's binding time. No
production fix was added by this review. The other findings and complete-suite
counts retain their original 2026-10-28 validation scope.

## Verification boundaries

The original complete-suite validation uses a freshly built Release runtime
from BG `8058a013` and test objects rebuilt against that revision's headers,
with matching `NDEBUG`.
This checks correctness; it is not a performance measurement.

| Run | Result |
| --- | --- |
| Ordinary engine suite | 2,254 cases; 2,941,855 assertions passed. |
| Ordinary library suite | 451 cases; 1,352 assertions passed. |
| Logging startup cases, each in a separate process | 4 cases; 21 assertions passed. |
| Hidden engine reproductions | 10 cases; 10 assertions failed with the recorded output or wiring error. |
| Hidden library reproduction | 1 case; 1 assertion failed during switch wiring. |
| Engine and library coverage-tag validation | Passed. |

Commands used for the original validation in the review lane:

```sh
build/verification/bg_tests
build/verification/bg_library_tests
build/verification/bg_tests '[repro]'
build/verification/bg_library_tests '[repro]'
```

The two reproduction commands intentionally return a failing status. Logs and
coverage reports are in the lane's `build/verification` directory, including
`latest-full-engine-final.log`, `latest-full-library-final.log`,
`latest-all-engine-repros-final.log`, and `latest-library-repros.log`.
The branch is `agent/hgraph-pattern-review`. Only tests and development reports
changed; the review has not been merged into the root worktree.

The review ports selected patterns to BG and runs BG tests. It does not claim
to rerun either complete hgraph suite, establish Python authoring parity from a
C++ test, or measure runtime performance. Source parameterizations and new BG
test cases are counted separately.

The post-library-merge check rebuilds the Release runtime and the map test
translation unit against `c8b33979`. All **39 map cases and 53 assertions** pass,
including the promoted retargeting regression. Its explicit filter also passes
independently. The complete engine and library suites were not rerun on this
revision. Commands and logs are in the retargeting investigation.

Failing reproductions use hidden Catch2 cases tagged `[.][repro]`. The default
suite excludes them. They retain their required assertions and have explicit
commands in the relevant report. Passing tests run in the ordinary suite.

## Reduction semantics

These source expectations are not used as BG failure oracles.

| Source expectation | BG contract | Classification |
| --- | --- | --- |
| 0.5 `_wiring/test_reduce.py::test_tsd_reduce_no_zero` expects `0` before any dictionary tick and after removing the final item. | Without a default, an invalid TSD or a valid TSD without valid items produces an invalid result. | Semantic difference. |
| 0.8 `ported/_wiring/test_reduce.py::test_tsd_reduce_zero_is_used_only_for_empty_and_singleton` expects `111` for singleton `1`, explicit zero `10`, and the associative combiner `left + right + 100`. | A singleton is forwarded directly, so BG produces `1`; the default is used only when the valid TSD has no valid items. | Semantic difference. |

## Scope exclusions

These source patterns require functionality outside the current BG contract.
They were inspected to identify the requirement and were not executed as BG
reproductions. The reduce exclusions below identify restrictions of the current
reduce API, not missing support for their constituent map, switch, bundle, or
tuple operations. They do not establish that these restrictions should remain.

| Source test | Required functionality | Disposition |
| --- | --- | --- |
| 0.5 `_wiring/test_map.py::test_tsl_map_wiring`, `test_tsl_map_wiring_no_key`; 0.5/0.8 `_wiring/test_reduce.py::test_tsl_reduce*` | Higher-order map/reduce over fixed TSL | BG's [map](../design/map.md) and [reduce](../design/reduce.md) operate on TSD. Fixed TSL storage and free TSL construction have separate tests. |
| 0.5 `_wiring/test_reduce.py::test_reduce_map`; 0.8 `test_reduce_zero_semantics.py::test_reduce_mapped_bundle_collection_projection_dereferences_key_set_source` | Reduce `TSD<K, TSD<J, TS<V>>>` into a TSD using a mapped reducer. | Current reduce wiring rejects collection-valued items before wiring the reducer. The map operation itself is supported. |
| 0.5 `_wiring/test_switch.py::test_reduce_from_switch` | Reduce mapped `TSB` items containing switch outputs into one TSB, then select a field. | Current reduce wiring rejects TSB-valued items. Map, switch, free bundle construction, and field selection are supported. |
| 0.5 `_wiring/test_reduce.py::test_reduce_tuple` | Ordered fold of each `TS<tuple<int, ...>>` tick into a string, starting from a `TS<string>` seed. | Current reduce accepts a TSD, requires matching item/result scalar types, and accepts a scalar default. This source case requires a scalar-collection fold, a different accumulator type, and a time-series seed. Tuple values themselves are supported. |
| 0.8 `test_reduce_zero_semantics.py::test_ordered_reduce_retains_delayed_source_schedules` | Ordered, non-associative reduction and a time series zero. | BG reduce requires an associative and commutative operation and accepts a scalar default. |
| 0.8 `ported/_wiring/test_context.py::test_context_crosses_stacked_try_except_boundaries` | A graph boundary that captures exceptions as output | BG [exception handling](../design/evaluation.md#exceptions) stops the graph and propagates the exception. No `try_except` operation was found in the public API. |
| 0.8 `test_distributed_map.py::test_state_churn_and_quiet_cycles`, `test_default_hosting_uses_a_separate_process` | Worker processes and distributed graph execution | The public BG map API runs local inner graphs. Local map lifecycle coverage does not establish distributed execution. |

Checkpoint recovery, external adapters, rolling-window operators, and the full
operator-by-reference Cartesian product are outside this round. Services,
feedback, and Python-specific conversion cases that were not explicitly mapped
in a lane report remain unverified by this review. Existing functionality is
not classified as unsupported solely because it was not selected here.
