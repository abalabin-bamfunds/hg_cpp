# Frame foundation handoff

Updated: 2026-10-14. The active lane is based on integrated main `a16f4761`.
The generic Row/RowPatch foundation checkpoint is `bd6d9cc5`. Commit `8e6bd52d`
separates logical row identity from physical slots; `69d1f643` retains native
owner/domain validation for borrowed views. Mixed computed/view TSF outputs and
standalone computed TSC outputs are implemented. Arrow-on, Arrow-off, and
ASan/UBSan consumer suites pass. Computed TSC was committed as `33a660d8`.
Independent review found and this lane fixes empty-domain publication on a
row-domain change.

## Work locations

[CONSOLIDATION.md](CONSOLIDATION.md) records the integrated BG changes, archive,
completed lane retirement, and consumer verification.

| Work | Location / branch | State |
| --- | --- | --- |
| Main | `/home/abalabin/_bg`, `main` at `a16f4761` | Scalar copy, output-runtime selection, and borrowed inputs integrated; persistent runtime inputs removed |
| Active frame work | `.lanes/frame-field-access`, `agent/frame-field-access` | Based on main `a16f4761` |
| Numeric value extraction | Separate work | Held; frame does not depend on it |
| Historical frame work | `extensions/frame/archive/` | Proposals, probe sources, and benchmark results preserved; see its README |
| Schema field reflection | Main | Integrated and covered by BG schema reflection tests |

The standalone consumers use `/home/abalabin/_bg/build/frame-dependency/install`
for BG, ddvisitor, and tpack. Conda provides unrelated dependencies.

Preserve the existing recovery stashes:

```text
6c9a333b5b819b453026f02beee198a7a32c4946
16cbdc77d65d43478e17eccff1f5930dca630415
f650a30a79bd707b8f566242c7bbe2d48020e578
```

Root retired eight superseded frame checkouts and branches, plus the scalar-copy
and output-runtime integration checkouts and branches. The active frame lane is
`.lanes/frame-field-access`. All 11 recovery stashes remain unchanged. The
recovery archive and retirement manifest are under
`/home/abalabin/_bg/build/frame-lane-recovery/2026-10-14`.

## Selected design

[README.md](README.md) defines node and value APIs.
[schema_foundation_proposal.md](schema_foundation_proposal.md) defines the
schema migration and extension boundary.

- `Frame<Schema>` and `Column<T>` are scalar declarations. `TSF<Schema>` and
  `TSC<T>` add row identity and current-cycle changes.
- Concrete and runtime schemas use one native layout and mutation implementation,
  one view implementation, and one Arrow frame implementation.
- Runtime schemas resolve during wiring and remain fixed for the graph lifetime.
- Current cells are nullable `i64` and `double`. All columns are present;
  optional schema fields are rejected independently of nullable cells.
- Internal recycled 32-bit row IDs are separate from user index columns.
  Removal reserves an ID through its removal cycle. Physical slots can be
  reused earlier. Insertion order is not guaranteed after removal.
- Membership changes publish every child column. Cell patches publish changed
  columns. Removed deltas expose IDs; previous cell values are outside scope.
- Scalar views borrow source storage during a node invocation. Scalar snapshots
  retain current values, nulls, row count, and the actual schema. Row IDs and
  deltas remain time-series state.
- Base scalar annotations preserve Derived owners and columns. Projection
  explicitly selects a narrower schema.
- Immutable native snapshot assignment retains its owner. Assignment from a
  mutable TSF borrow creates an independent snapshot.
- Typed `Row<Schema>` inserts a complete row. Typed `RowPatch<Schema>` supports
  insertion and modification. Insertion sets omitted fields to null;
  modification leaves omitted fields unchanged. An empty insertion patch
  adds an all-null row; an empty modification patch publishes nothing.
- Generic mutation also accepts complete schema-order scalar arguments and named
  sparse patches. Validate shape, names, and cell types before mutation.
  Argument wrappers are temporary; tables do not store per-cell scalar handles.
- `Field<SCALAR>` exposes a generic column. `view<T>()` checks its resolved type.
- Projection binds ordinals in target-schema order. Named and compile-time
  conveniences use the same binding operation.
- Live filtering belongs to a node. Expression evaluation operates on blocks;
  the node applies row-selection changes to a TSF view output. Expressions use
  BG materialisation options. Do not add the proposed public cell callback API.

## Implemented foundation

BG shared reflection provides field construction, offsets, and logical field
access for concrete, inherited, and shadowed schemas. Its `FieldProbe` captures
field addresses from construction of the actual schema type. BG provides
alternative scalar reads, borrowing, canonical runtime selection, and the
`runtime_type_for_copy` selector. These are extension-facing operations;
node code uses `In`, `Out`, `Scalar`, and `ScalarValue`.

Frame storage uses `NativeFrameStorage` with a resolved native layout.
Typed wrappers translate arguments and access columns. The shared view storage
keeps child states, column mappings, and modified-row indexes at fixed offsets.
Ordinary incremental bindings process deltas without traversing every row.

`NativeRows` owns logical row identity and membership deltas. `NativeRowSlots`
owns physical slot occupancy and the two ID/slot mappings. A computed column
can use local slots with the source logical RowSet and explicit deltas. Native
bindings retain their owner's row domain; local-slot bindings have no native
logical owner. Scalar and time-series traversal preserve visible source order
independently of local slot order.

Mixed outputs use `Out<TSF<Schema>, frame::computed<"mid">>`. The shared
runtime-schema view backend owns computed cell buffers and one local slot map;
remaining columns retain source bindings. `bind(source)` matches borrowed
columns by name and type and applies membership changes directly to computed
storage. New computed cells start null. `set<Name>` and `set_batch<Name>` update
computed cells and publish their child columns. Batch preflight completes before
cell writes and publication occurs once. Set-only later invocations refresh the
column delta timestamp. Accepted equal-value writes follow ordinary semantics.

Standalone computed columns use `Out<TSC<T>, frame::computed<>>`. `bind(source)`
tracks source membership and row identity while owning result cells. Source cell
changes alone do not publish the result. `set` and `set_batch` write result
cells; new rows start null, and removed rows release their local slots. The
result cell type may differ from the source cell type. Generic declarations
resolve source and result cell types at wiring.

The TSF scalar adapter borrows persistent output storage. It is not registered
as default scalar storage. The default is the immutable snapshot runtime,
which stores one `FrameSnapshotOwner*`. Owners retain their actual schema,
columns, allocator, and matching BG allocation owner.

Generic `ColumnView`, `Column`, TSC and TSF inputs/outputs, and scalar factories
use the existing numeric implementations. Generic inputs expose actual schemas,
columns by name or ordinal, and checked typed access. Time-series cells use row
IDs; scalar-view cells use positions. Runtime-schema writers accept borrowed
scalar spans and concrete Row/RowPatch arguments. Concrete row adapters pass
native nullable-cell pointers without scalar serialization. Generic scalar
arguments use temporary numeric staging and checked scalar reads.
Exact concrete Row/RowPatch operations use compile-time pointer arrays and
perform no schema registration or field-name lookup.

Row/RowPatch fields declared with a scalar variable accept borrowed `Scalar<>`
values. These fields use `optional<Scalar<>>`; concrete numeric
fields retain `optional<T>`. Generic row adapters validate the complete source
schema against the resolved output schema using BG type matching, then decode
present borrowed handles into temporary numeric values before mutation.
Numeric staging remains alive through the native mutation call.
Constrained and repeated variables retain their normal matching rules.
Tables never retain the argument handles. Null and omitted patch behavior
follows the selected Row/RowPatch rules above.

Arrow imports share one runtime-schema backend. Inferred and explicit schemas
use BG descriptors. Borrowed exports share compatible value buffers and may
allocate chunk metadata and validity bitmaps. `copy_to_arrow` gathers independent
owning buffers for off-graph use. Typed owning copies select native snapshots;
erased Arrow copies preserve Arrow representation.

## Verification

Coverage includes native and runtime schemas, numeric columns, snapshots,
borrowing, Arrow views and copies, projection, and concrete and generic
Row/RowPatch writes. Tests cover schema/type validation before mutation,
nullable cells, all-null insertions, inherited and shadowed fields, generic
type variables, and ownership after graph destruction. Borrowed graph input
tests use native and view TSF runtimes with one concrete schema. They verify
runtime descriptors, source and column buffer identity, nullable cells, and
row and column deltas across invocations. Test checkpoint: `bf4d9483`.

BG main `6051cc63` previously passed its complete C++ and Python suite:
1,720/1,720. Current verification uses a refreshed Debug BG installation built
from `a16f4761` in `/home/abalabin/_bg/build/frame-dependency/bg-build`, with
testing, Python, and benchmarks disabled. The complete BG suite was not repeated.
Its build and install logs are `build-a16f4761.log` and
`install-a16f4761.log` under `build/frame-dependency` in the root checkout.
The rebased frame lane and row-slot commits passed all five Arrow-enabled
Release suites against this installation.

All three consumer caches resolve BG, ddvisitor, and tpack from the installed
prefix. Conda supplies unrelated dependencies including Arrow 24.0.0. Arrow-on
Release regenerated its CMake configuration during its build. The other two
consumers were explicitly reconfigured with their existing settings.

Previous consumer verification at main `bf338ca5`:

| Consumer | Build | Test |
| --- | --- | --- |
| build/field-access-on, Release, Arrow enabled | Passed with -j12 | 5/5 suites passed |
| build/cc4-checkpoint-off, Release, Arrow disabled | Passed with -j12 | 4/4 suites passed |
| build/cc4-lifetime, Debug, Arrow enabled, ASan/UBSan | Passed with -j12 | 5/5 suites passed; no sanitizer diagnostics |

The sanitizer test used ASAN_OPTIONS=detect_leaks=0 and
UBSAN_OPTIONS=halt_on_error=1. Leak detection remains disabled because of
environment ptrace restrictions. The sanitizer frame build links the Debug BG
dependency. Clang and MSVC have not been tested.

The previous build and test logs are `build-bf338ca5.log` and `test-bf338ca5.log` in
each consumer build directory. Explicit configure logs are
`configure-bf338ca5.log` in `cc4-checkpoint-off` and `cc4-lifetime`.
Earlier verification logs remain preserved. No retired-worktree paths occur
in the three consumer caches. The Arrow-on Release shared-library RPATH uses
the installed prefix and Conda libraries.

Run builds and tests from the active lane:

```sh
conda run --no-capture-output -n bg-build cmake --build extensions/frame/build/field-access-on --parallel 12
conda run --no-capture-output -n bg-build ctest --test-dir extensions/frame/build/field-access-on --output-on-failure
```

Use `cc4-checkpoint-off` for Arrow-off Release. For `cc4-lifetime`, prefix the
CTest command with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`.

## Performance artifacts

The preserved active-lane sample is archived at:

```text
extensions/frame/archive/benchmarks/foundation-benchmark/shared-view.md
extensions/frame/archive/benchmarks/foundation-benchmark/shared-view.csv
```

The workload starts with 100,000 rows and runs source mutation, filtering,
selection, and sum. Allocation and destruction are included. The extension
uses Release; installed BG uses Debug. Median borrowed dense Arrow export plus
sum was 1.255 ms. Exporting 49,168 scattered chunks plus sum was 15.857 ms;
gathering that selection plus sum was 1.483 ms. These measurements predate the
current API completion.

The earlier schema-hooks comparison and direct sparse-bunch PyArrow probe are
preserved under `extensions/frame/archive/`. Their source and measurement
conditions are documented in the archive README. These results inform the
design; they do not replace the consumer verification recorded above.

## Status and remaining work

Fixed-name `Out<TSF<Schema>, frame::computed<"mid">>` outputs are implemented
through the shared runtime-schema view backend. Dedicated Arrow-on tests cover
concrete and runtime schemas, independent computed buffers, shared slot maps,
row removal and reuse, null batches, validation before mutation, downstream
views, set-only later invocations, cache identity, and child revalidation.

Standalone computed TSC tests cover distinct and generic source/result types,
row membership changes, same-cycle writes and removals, null writes, batch
preflight, invalidation, domain rebinding, and row-ID reuse. Plain-view
regression tests cover mapping changes with missing retained coverage and
removal after a same-cycle change. Empty-domain graph tests cover plain and
computed TSC bindings, all-computed TSF bindings, and a selected TSF child.

Independent review found that rebinding between distinct empty row domains
could update the exposed domain without publishing. This correction publishes
the domain change while keeping row deltas empty. Graph tests cover computed
TSC, all-computed TSF, and a selected TSF child; same-domain empty bindings
remain unticked. Root reviewed the correction and test fixtures.

The declaration for computed TSF field names selected at wiring remains
undecided; standalone computed TSC outputs use the approved `computed<>`
declaration. Exposing BG's existing integer-division helper also remains
undecided; do not duplicate its implementation.

The two BG changes integrated in this task are scalar copy runtime selection
and wiring-time output runtime selection. The frame extension uses scalar copy
selection for owning frame copies and the output hook to select frame layouts.
Frame output layout options remain extension-owned.

The shared numeric function extraction remains held. Frame expression and
block-arithmetic design are future work; their design will determine whether
that extraction is reused.

After the empty-domain publication correction, Arrow-on Release passed all 5
suites in `build/field-access-on`; Arrow-off Release passed all 4 suites in
`build/cc4-checkpoint-off`; and Arrow-on Debug with ASan/UBSan passed all 5
suites in `build/cc4-lifetime`, using `ASAN_OPTIONS=detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1`.

Previous-value subscriptions, strings and symbols, compaction, capacity
trimming, and delta transport remain later work. Previous-cell capture, if
needed, is requested from the owning storage through ColumnSource; graph
back-propagation is not required. For expressions with time-series scalar
parameters, a separate expression-construction node is intended to simplify
node code. These features are not implemented.
