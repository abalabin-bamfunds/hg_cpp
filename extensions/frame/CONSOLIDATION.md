# Frame lane consolidation review

Reviewed on 2026-10-14 against main `052a7f0b`.
This review proposes integration order. It does not merge, rebase, or remove work.

## Target locations

- BG extension support and shared numeric functions belong on main.
- The ongoing frame implementation belongs in `.lanes/frame-field-access`,
  branch `agent/frame-field-access`.
- Historical frame experiments and their documentation belong under that
  extension's `experiments/` directory, with their original revision recorded.

## BG changes to integrate

| Change | Source | Current state | Frame use |
| --- | --- | --- | --- |
| Owning scalar copy runtime selection | `.lanes/scalar-access-paths`, `a8af8eef` | Committed; one uncommitted test correction | Current native snapshots and erased Arrow copies |
| Shared numeric value functions | `.lanes/library-numeric-values` | Uncommitted; new `include/bg/library/numeric.hpp` is untracked | Next arithmetic implementation shares BG's numeric behavior |
| Wiring-time output runtime selection | `.lanes/output-runtime-hook` | Uncommitted, reviewed and tested | Mixed computed/view frame output layouts, including runtime schemas |

The copy change adds `ScalarRuntimeType::runtime_type_for_copy` and its owning
storage integration. Include the test correction in
`tests/value/scalar/storage_traits_test.cpp`: an owning provider copy uses its
selected snapshot runtime.

The numeric change extracts `bg::library::numeric::{add, sub, mul, div}` for
`i64` and `double`. Scalar nodes use the same implementations. Preserve its
untracked header along with the three tracked source, test, and documentation
changes.

The output hook accepts `const WiringArguments &` on aggregate, callable, and
runtime-signature node definitions. Its selected runtime type controls output
allocation and `Out` construction. Without a hook, selection remains unchanged.
The `WiringTemplate::wire` interface change belongs with this implementation.
Frame-specific output specifications remain in the extension.

All three sources are behind current main. Integrate their task changes onto
current main and test them together. Do not restore complete old file trees over
main: those trees lack newer TSD and reference changes.

### Already integrated BG support

Main contains shared schema descriptors, exact-type schema field reflection,
type-name formatting, source-aware scalar borrowing and reads, provider-owned
time series support, sparse storage block/run access, and sampled collection
item ID remapping. Old lane implementations of these features are not new frame
dependencies.

### Existing verification

These results apply to each lane's recorded base, not to the proposed combined
integration:

| Change | Recorded result |
| --- | --- |
| Scalar copy selection | Complete suite: 1,649/1,649 |
| Shared numeric functions | Complete build; arithmetic tests: 45/45 |
| Output runtime hooks | Complete C++/Python build and suite: 1,714/1,714 |

## Frame work to retain together

`frame-field-access` has 14 extension commits beyond its base and current
uncommitted foundation work. Keep both. The foundation includes concrete and
runtime schemas, native/view/Arrow storage, snapshots, borrowing, projection,
Row/RowPatch mutation, and Arrow view/copy export. [HANDOFF.md](HANDOFF.md)
records the implementation and verification.

| Old lane | Audit result | Before removing its checkout |
| --- | --- | --- |
| `frame-schema` | Its 14 commits are patch-equivalent to the active frame series; dirty source is an older checkpoint | Preserve its benchmark comparison artifacts |
| `frame-extension` | Earlier untracked extension snapshot; no unique committed work | Verify retained source against the active frame tree |
| `frame-fixed-schema` | Earlier untracked extension snapshot; no unique committed work | Verify retained source against the active frame tree |
| `frame-views` | Earlier untracked extension snapshot; no unique committed work | Verify retained source against the active frame tree |
| `frame-output-options` | Earlier untracked extension snapshot; no unique committed work | Verify retained source against the active frame tree |
| `frame-utilities` | Tracked utility changes and untracked header match main | Preserve its recovery checkpoint until consolidation verification |
| `frame-arrow-probe` | Unique untracked experiments and original design proposals | Carry the experiment directory into the active frame lane |

Preserve the complete `frame-arrow-probe/experiments/frame_arrow_probe/`
directory as historical research. It contains the low-level PyArrow probe,
sparse benchmarks, results, and original proposals, including
`utility_api_proposal.md`. Mark these proposals as historical so they are not
mistaken for the current API. Adjust the scripts' repository-relative paths when
moving the experiment, and verify the moved files.

The earlier benchmark separates Arrow array construction from kernels over
prebuilt arrays. The active extension benchmark measures complete frame
export/gather and consumer operations. Keep both measurements with their
workload descriptions.

The comparison artifacts in
`frame-schema/extensions/frame/build/schema-hooks-consumer` also need preservation:
`baseline.csv`, `baseline.md`, `after-storage.csv`, and `after-storage.md`.
Keep both result pairs together and record branch `agent/frame-schema`, commit
`1b78c7f9`, and their origin in an ignored build directory.

The `schema-field-probe` checkout no longer exists. Its production implementation
is already in main at `8f4dc0a9`, with tests in
`tests/utils/schema_reflection_test.cpp`. The related standalone source
`frame-schema/extensions/frame/build/offset-probe/offset_probe.cpp` still exists
and is absent from the active lane. Preserve it as historical schema reflection
research before removing `frame-schema`.

## Build dependency to remove

All three active standalone frame build caches use BG installed under
`scalar-access-paths/build/dev/install-a8af8eef`. Their cached `ddvisitor_DIR` and
`tpack_DIR` also refer to `install-cc4a3f29` in that lane.

After BG integration, install main into one prefix. Reconfigure the frame's
Arrow-on, Arrow-off, and sanitizer consumers against that prefix, resolving BG
and its installed dependencies together. The scalar lane cannot be removed
while these caches depend on it.

## Consolidation order

1. Preserve the uncommitted BG work, including untracked files. Reuse one BG
   integration lane for the three changes.
2. Rebase, review, and test the combined BG changes against current main. Present
   the result for integration approval as required by `LOCAL_AGENTS.md`.
3. Preserve and commit the current frame foundation in its lane; rebase it onto
   integrated main. Keep the extension on its branch.
4. Install integrated main and reconfigure/test all three frame consumers.
5. Carry historical experiments and comparison artifacts into the frame lane.
   Record their source and verify that no unique files are omitted.
6. Remove superseded checkouts only after source, documentation, artifacts, and
   recovery checkpoints are accounted for and removal is approved.

Keep recovery stashes intact during this work. Other scalar visitor, input
binding, TSD, and library lanes are separate work; this frame review does not
authorize their integration or deletion.
