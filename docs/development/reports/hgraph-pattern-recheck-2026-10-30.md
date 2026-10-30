# hgraph pattern recheck, 2026-10-30

The review lane `agent/hgraph-pattern-review` was rebased onto the fixed BG
baseline `2de7d53f`. The recheck builds the nine affected test translation
units against that baseline's headers and runs them against its Release
libraries. Production headers and source match `2de7d53f`. Later `main` commits
were not tested. The recheck changes tests and reports only.

## Remaining findings

Four hidden engine cases remain, grouped into two findings.

| Finding | Cases | Result |
| --- | ---: | --- |
| A nested graph returns an explicit `REF<TSB<...>>` or `REF<TSL<...>>` for a free structure, then a downstream value consumer samples that result. | 2 | Both fail during wiring with `a time series reference cannot target another reference`. Runtime behavior is not reached. |
| A mapped switch/reduce composition does not publish the default result at its initial empty state. | 2 | The exact empty-dictionary case and the initial invalid-item case both fail the expected whole-sequence comparison. The invalid-item variant was observed as `SKIP, 2, 5, 6, 7, 4`; the exact empty-dictionary output was not separately decoded. The map and reducer validity states remain unresolved. |

The mapped switch/reduce composition passes for a populated initial dictionary,
producing `2, 5, 6, 7, 4`. This confirms the populated path while leaving the
initial default path unresolved. A diagnostic observer compiled but failed at
runtime with `the requested time series representation is not implemented`
when it read the mapped TSB references. It did not alter the original graph
composition or provide validity observations.

## Upstream pattern mapping

| Upstream pattern | BG case | Current result |
| --- | --- | --- |
| 0.5 `test_tsl_ref_flipping` | `a reference can select among free fixed lists and become invalid` | Pass. The rebuilt-list reselection case remains a separate failure on the previously recorded baseline. |
| 0.5 `test_reduce_map` | `reduce maps and combines nested dictionaries` | Pass. |
| 0.8 `test_reduce_mapped_bundle_collection_projection_dereferences_key_set_source` | `reduce maps a projected bundle of nested dictionaries` | Pass. |
| 0.8 `test_map_preserves_typed_keyed_bundle_reference_and_materialization` | `map preserves keyed bundle references and materializes a distinct schema` | Pass. |
| 0.5 `test_mesh` | `reduce combines mesh outputs as branches update and disappear` | Pass. |
| 0.5 `test_mesh_2` | `mesh branches reduce nested dictionaries before using their dependencies` | Pass. |
| 0.5 `test_mesh_removal` | `mapped meshes reduce independently and rebuild after a parent returns` | Pass. |
| 0.5 `test_reduce_from_switch`: empty dictionary then updates | `reduce combines mapped bundles containing switch outputs from an empty dictionary` | Hidden repro; expected whole-sequence comparison fails. |
| BG additional corner: invalid TSD item then updates | `reduce combines mapped bundles containing switch outputs when an item is invalid` | Hidden repro; expected whole-sequence comparison fails. |
| 0.5 `test_reduce_from_switch`: populated initial dictionary | `reduce combines populated mapped bundles containing switch outputs` | Pass. |
| 0.5 `test_mesh` and `test_mesh_2` switch-containing variants | `switch-selected mesh branches feed a reduction`; `a reduction observes mesh dependencies after switch outputs change` | Both pass after the test graph callbacks gained concrete `.as<TS<i64>>()` return annotations. These two existing repros are promoted to ordinary tests. |

The populated map/switch/reduce case passes with `2, 5, 6, 7, 4`. The native/free
and rebuilt-free bundle switch cases, free bundle/list activation cases,
rebuilt-list item identity case, and `take` composition also pass. The map
retarget regression continues to pass.

## Historical suite failures

The earlier complete-suite run recorded a hierarchical scalar conflict failure
and Python web binding failures. This focused recheck does not rerun the full
suite, so those failures remain historical and unrechecked on `2de7d53f`.

## Verification

All nine review test translation units compile with GCC Release flags and
`NDEBUG`. The focused engine and library executables load lane-local copies of
the Release libraries from `build/recheck/runtime`; their hashes match the
libraries in `build/current-release`. Compile commands, object files, library
hashes, and run commands are recorded in `build/recheck/current-manifest.json`.
The manifest pins verification to `2de7d53f`, even if `main` advances.

| Run | Cases | Result |
| --- | ---: | --- |
| Ordinary engine cases in the affected test files | 230 | All pass. |
| Ordinary library cases in the affected test files | 72 | All pass. |
| Hidden engine reproductions | 4 | All fail: two nested REF wiring cases and two initial-default variants. |

The hidden engine cases retain their assertions and use `[.][repro]`. The
ordinary library run includes the `take` composition and passes it. There are no
hidden library reproductions in the current test set.
