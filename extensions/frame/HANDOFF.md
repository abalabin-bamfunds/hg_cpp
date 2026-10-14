# Frame foundation handoff

Updated: 2026-10-14. The frame foundation includes generic Row/RowPatch field
support. It is based on integrated main `6051cc63`; the foundation checkpoint
`0cb29949` records the completed row work. The active lane has been rebuilt and
tested against the integrated main install.

## Work locations

[CONSOLIDATION.md](CONSOLIDATION.md) records the integrated BG changes, archive,
completed lane retirement, and consumer verification.

| Work | Location / branch | State |
| --- | --- | --- |
| Main | `/home/abalabin/_bg`, `main` at `6051cc63` | Scalar copy and output-runtime changes integrated |
| Active frame work | `.lanes/frame-field-access`, `agent/frame-field-access` | Based on main `6051cc63` |
| Numeric value extraction | Separate work | Held; frame does not depend on it |
| Historical frame work | `extensions/frame/archive/` | Proposals, probe sources, and benchmark results preserved; see its README |
| Schema field reflection | Main | Integrated and covered by BG schema reflection tests |

The standalone consumers use `/home/abalabin/_bg/build/frame-dependency/install`
for BG, ddvisitor, and tpack. Conda provides unrelated dependencies. Each
consumer cache was cleared of the prior package paths before reconfiguration.

Preserve the recovery stashes, including the checkpoint made before this rebase:

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
type variables, and ownership after graph destruction.

BG main 6051cc63 passed its complete C++ and Python suite: 1,720/1,720.
The three frame consumers were configured with
CMAKE_PREFIX_PATH=/home/abalabin/_bg/build/frame-dependency/install;/home/abalabin/miniforge3/envs/bg-build.
Each configure removed cached bg_DIR, ddvisitor_DIR, and tpack_DIR before
package discovery. All three packages resolve from the installed main prefix;
Conda supplies unrelated dependencies including Arrow.

| Consumer | Build | Test |
| --- | --- | --- |
| build/field-access-on, Release, Arrow enabled | Passed with -j12 | 5/5 suites passed |
| build/cc4-checkpoint-off, Release, Arrow disabled | Passed with -j12 | 4/4 suites passed |
| build/cc4-lifetime, Debug, Arrow enabled, ASan/UBSan | Passed with -j12 | 5/5 suites passed; no sanitizer diagnostics |

The sanitizer test used ASAN_OPTIONS=detect_leaks=0 and
UBSAN_OPTIONS=halt_on_error=1. Leak detection remains disabled because of
environment ptrace restrictions. Clang and MSVC have not been tested.

Configure, build, and test logs are stored in each consumer build directory as
reconfigure-main.log, build-main.log, and test-main.log. A scan of all three
build trees found no references to retired worktrees. Frame shared-library
RPATHs point to the installed main prefix and Conda libraries, plus the local
frame build directory for the Arrow adapter. Root reran all three CTest
configurations after lane retirement: Arrow on 5/5, Arrow off 4/4, and ASan/UBSan
5/5, with no sanitizer diagnostics. Those logs are named
root-after-retirement-ctest.log in each consumer build directory.

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

## Agreed next work

The two BG changes integrated in this task are scalar copy runtime selection
and wiring-time output runtime selection. The frame extension uses scalar copy
selection for owning frame copies. The output hook is available for planned
mixed computed/view output layouts. Frame output layout options remain
extension-owned.

The shared numeric function extraction is held. Block arithmetic design and
implementation are next. Whether that work should reuse the held extraction
depends on the design and implementation.

Previous-value subscriptions, strings and symbols, compaction, capacity
trimming, and delta transport remain later work. Previous-cell capture, if
needed, is requested from the owning storage through ColumnSource; graph
back-propagation is not required. For expressions with time-series scalar
parameters, a separate expression-construction node is intended to simplify
node code. These features are not implemented.
