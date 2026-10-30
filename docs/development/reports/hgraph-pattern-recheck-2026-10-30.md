# hgraph pattern recheck, 2026-10-30

This report preserves the focused recheck on BG baseline `2de7d53f` and records
the later integrated verification after the nested-reference and switch-output
corrections. The original counts and failures remain pinned to `2de7d53f`.

## Historical findings on `2de7d53f`

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

| Upstream pattern | BG case | Result on `2de7d53f` |
| --- | --- | --- |
| 0.5 `test_tsl_ref_flipping` | `a reference can select among free fixed lists and become invalid` | Pass; BG asserts explicit invalidation where upstream uses `None`. |
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

## Historical suite failures at `2de7d53f`

The earlier complete-suite run recorded a hierarchical scalar conflict failure
and Python web binding failures. This focused recheck does not rerun the full
suite, so those failures remain historical and unrechecked on `2de7d53f`.

## Historical verification on `2de7d53f`

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

The hidden engine cases retained their assertions and used `[.][repro]` in this
historical run. The ordinary library run included the `take` composition and
passed it. There were no hidden library reproductions in that test set.

## Current integrated verification

The integrated lane contains root revision `1ad2adff`, switch output
compatibility and case-lifetime corrections, and the nested reference-valued
output correction. The nested correction is commit `a391afff`; the lane head
before these test and report updates is `62b94622`. Verification uses headers
and matching libraries built from this integrated tree.

All four cases from the historical findings now pass as ordinary tests. The
nested free bundle and fixed-list REF outputs pass through the nested graph to
their value consumers. The two mapped switch/reduce cases expect `SKIP` at the
initial empty or invalid-item state, followed by the same values as before:
`2, 5, 6, 7, 4`. BG does not publish a value tick at those initial states. The
tests assert five emitted events and preserve their original graph inputs and
inherited bundle schema; the redundant copies in `nested_graph_test.cpp` were
removed.

The integrated verification builds and runs nine engine test files and two
library test files. It does not run the complete test suite or the optional
diagnostic observer. A later [diagnosis](map-switch-reduce-first-tick-2026-10-30.md)
traced the observer's SIGSEGV to its node-template lifetime. That separate
diagnostic run is not part of these test counts.

| Target | Test files | Cases | Assertions | Result |
| --- | ---: | ---: | ---: | --- |
| Focused engine runner | 9 | 270 | 405 | All pass. |
| Focused library runner | 2 | 77 | 162 | All pass. |

The focused runs contain 347 cases and 567 assertions. The runner executables
are `build/integration/hgraph-pattern-engine-tests` and
`build/integration/hgraph-pattern-library-tests`. Both load the matching
lane-local Release libraries from `build/current-release`.

Build and run commands from the lane root:

```sh
cmake --build build/current-release --target bg bg_library -j 4
cmake --build build/integration/cmake --target hgraph-pattern-engine-tests -j 4
cmake --build build/integration/library-cmake --target hgraph-pattern-library-tests -j 2
build/integration/hgraph-pattern-engine-tests --reporter compact
build/integration/hgraph-pattern-library-tests --reporter compact
```

The focused runner project is stored in the ignored `build/integration`
directory. Runtime and executable hashes from this verification are:

| File | SHA-256 |
| --- | --- |
| `build/current-release/libbg.so` | `30443b271d4f61e6b87837ed9decad5cdd1d8829271adb45c09ddb296a03e2b9` |
| `build/current-release/libbg_library.so` | `1c588c02436e31bcdb7ecea38ac0faa736c9cd208bfcaab2756b83edee49d3e8` |
| `build/integration/hgraph-pattern-engine-tests` | `99ae294c830ce43b05c6b1452f4dd94c338ce33ac87dbcc5591d5b13beb5ffec` |
| `build/integration/hgraph-pattern-library-tests` | `714b7bc268eb90b973357c095b46d37b90081cfdb26376cd47daee5705caec9c` |
