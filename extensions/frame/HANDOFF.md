# Frame foundation handoff

Updated: 2026-10-14. Generic Row/RowPatch field support is complete and reviewed.
The current source passed all configurations below. It remains uncommitted
and unmerged.

## Work locations

[CONSOLIDATION.md](CONSOLIDATION.md) records the lane review against main
`052a7f0b`, the three pending BG changes, and the proposed integration order.

| Work | Location / branch | State |
| --- | --- | --- |
| Main | `/home/abalabin/_bg`, `main` | Maintained separately; this frame task makes no root changes |
| BG copy selection support | `.lanes/scalar-access-paths`, `agent/scalar-access-paths` | Rebased at `a8af8eef` onto main `d06e719e`; unmerged |
| Active frame work | `.lanes/frame-field-access`, `agent/frame-field-access` | Rebased HEAD `c80c8d70` onto main `d06e719e`; substantial uncommitted foundation changes |
| Shared numeric value functions | `.lanes/library-numeric-values`, `agent/library-numeric-values` | Based on main `d05863a6`; approved, implemented, uncommitted |
| Wiring-time output-runtime hooks | `.lanes/output-runtime-hook`, `agent/output-runtime-hook` | Based on main `6680e192`; implemented, reviewed, tested; uncommitted and unmerged |
| Original frame checkpoint | `.lanes/frame-schema`, `agent/frame-schema` | Preserved at `1b78c7f9` with its working changes and artifacts |
| Schema field reflection | Main, `8f4dc0a9` | Integrated; the former `schema-field-probe` checkout no longer exists |

The standalone frame consumers use installed BG from:

```text
/home/abalabin/_bg/.lanes/scalar-access-paths/build/dev/install-a8af8eef
```

The installed package includes the unmerged scalar-copy hook from the
scalar-access-paths lane. Inspect that hook in its dependency lane.
Do not configure a root BG build in the frame lane.

Preserve the recovery stashes, including the checkpoint made before this rebase:

```text
6c9a333b5b819b453026f02beee198a7a32c4946
16cbdc77d65d43478e17eccff1f5930dca630415
f650a30a79bd707b8f566242c7bbe2d48020e578
```

Root owns design, documentation, user interaction, independent verification,
and integration. `/root/frame_generic_rows` completed the Row/RowPatch work
and froze its source before root verification.
The dirty checkpoint was preserved by stashing and restored after the rebase.
Do not reset it.
Commit and integration require owner authorization.

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

The approved generic field, immutable snapshot assignment, Row/RowPatch writer,
and projection APIs are implemented and tested. Integration coverage includes
runtime-schema mutation through public nodes feeding runtime-schema view outputs,
checked declared generic fields, null patches, validation before mutation,
zero-column frames, snapshot sharing, and ownership after graph destruction.
RowPatch insertion covers partial and all-null rows in concrete and generic
outputs. Writer tests check reordered fields, wrong counts and types, unknown
and duplicate names, and empty modification patches. Missing-row writes preserve
prior delta state. Projection validates ordinals and cell types even while its
source is invalid, before changing output validity.

Generic Row/RowPatch tests cover declared-generic outputs resolved to both
numeric types, runtime-schema outputs, mixed fields, inherited and shadowed
fields, compatible and incompatible constrained/repeated variables, and
borrowed-value independence. A rejected later patch field leaves earlier valid
fields unchanged. Complete source-schema validation includes omitted fields.

Before rebasing onto `d06e719e`, root rebuilt and tested the frozen source with
GCC C++23 on 2026-10-13, including generic Row/RowPatch field support:

| Configuration | Result |
| --- | --- |
| Release, Arrow enabled | 5/5 suites passed |
| Release, Arrow disabled | 4/4 suites passed |
| Debug, Arrow enabled, AddressSanitizer and UndefinedBehaviorSanitizer | 5/5 suites passed; no sanitizer diagnostics |

Existing standalone build directories, relative to the frame lane root:

```text
extensions/frame/build/field-access-on       Release, Arrow ON
extensions/frame/build/cc4-checkpoint-off    Release, Arrow OFF
extensions/frame/build/cc4-lifetime          Debug, ASan/UBSan, Arrow ON
```

Use `-j12`. Build and test from the frame lane:

```sh
conda run --no-capture-output -n bg-build cmake \
  --build extensions/frame/build/field-access-on --parallel 12
conda run --no-capture-output -n bg-build ctest \
  --test-dir extensions/frame/build/field-access-on --output-on-failure
```

Substitute `cc4-checkpoint-off` for the native-only consumer.
For the sanitizer consumer, use `cc4-lifetime` and:

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
conda run --no-capture-output -n bg-build ctest \
  --test-dir extensions/frame/build/cc4-lifetime --output-on-failure
```

LeakSanitizer is disabled due environment ptrace restrictions. Allocation-balance
and injected-failure tests remain enabled. GCC C++23 is available; Clang and MSVC
have not been tested. BG `cc4a3f29` passed its complete 1,547-test suite and
coverage validation before installation.

After the rebase, the dependency rebuilt successfully. Its full suite passed
1,649 of 1,649 tests, and core and library coverage validation passed. One new
main test needed its runtime expectation updated: wiring retains an erased
owning scalar copy, so the copy-selection hook selects the provider's immutable
snapshot runtime. The test still verifies the value and provider read function.
This test-only change remains uncommitted in the dependency lane. Production
behavior is unchanged. The full-suite log is
`.lanes/scalar-access-paths/build/dev/scalar-rebase-ctest.log`.

Root rebuilt and tested all standalone consumers after the rebase against the
new installed package:

| Configuration | Result |
| --- | --- |
| Release, Arrow enabled | 5/5 suites passed |
| Release, Arrow disabled | 4/4 suites passed |
| Debug, Arrow enabled, AddressSanitizer and UndefinedBehaviorSanitizer | 5/5 suites passed; no sanitizer diagnostics |

Leak detection remains disabled as described above. The package has unchanged
production code after the test-only correction.

Bounded independent reviews covered ownership, shared views, Arrow,
registration, generic field access, snapshot assignment, and writer/projection
APIs. The final recheck verified argument validation before output mutation and
the corresponding regressions. `git diff --check` is clean.

## Performance artifacts

Current artifacts are in the active lane:

```text
extensions/frame/build/foundation-benchmark/shared-view.md
extensions/frame/build/foundation-benchmark/shared-view.csv
```

The workload starts with 100,000 rows and runs source mutation, filtering,
selection, and sum. Allocation and destruction are included. The extension
uses Release; installed BG uses Debug. Median borrowed dense Arrow export plus
sum was 1.255 ms. Exporting 49,168 scattered chunks plus sum was 15.857 ms;
gathering that selection plus sum was 1.483 ms. These measurements predate the
current API completion.

Preserved comparison artifacts are under
`.lanes/frame-schema/extensions/frame/build/schema-hooks-consumer`.
Repeat performance testing only for a concrete risk introduced by the changes.

## Later work

The current request is to start expressions and computed columns. Register
frame arithmetic overloads on the exported `bg::operators` objects from
`<bg/library.hpp>` and link `bg::library`. C++ forwarding operators already
exist; do not define separate arithmetic operator identities or forwarding
operators.

The owner agreed that a compile-time output specification lists its computed
columns, for example `Out<TSF<Prices>, frame::computed<"mid">>`. Other columns
use view storage. Keep this configuration on `Out`, outside the semantic frame
type. This specification has not been implemented.

For runtime schemas, the owner requested a wiring-time output-runtime hook on
aggregate definitions and callable definitions. The descriptor must be selected
before BG compiles output storage. Runtime descriptors contain shared layout
metadata; buffers, rows, deltas, and actual source bindings belong to output
storage. The hook is implemented in `.lanes/output-runtime-hook`, branch
`agent/output-runtime-hook`, rebased onto main `6680e192`. The owner confirmed
that implementation can proceed in the separate lane. Aggregate definitions
provide a static `output_runtime_type(const WiringArguments &)` hook; callable
definitions use `bg::output_runtime_type = callback`; runtime-signature definitions
provide an instance hook. Each returns `const TimeSeriesRuntimeType &`.
The approved engine API replaces the `TypeMap` parameter of
`WiringTemplate::wire` with `const WiringArguments &`; its normalized argument
span remains separate. The view retains fixed and flattened variadic argument
descriptors without repeating normalization. The selector must return the same
runtime type for the same node definition and arguments. Node-call
deduplication retains its existing identity.
The complete C++ and Python CTest suite passed 1714/1714 on 2026-10-14 after
the concrete output binding correction. The complete build passed with `-j12`.
The build includes Python 3.13.15. Coverage validation reports six existing tag
errors in unchanged tests. The operator-cache regression verifies one overload
requirement call and two selector calls across different scalar configuration
values. Repeated identical calls retain one finalized output.

The selected runtime type controls both output allocation and `Out` construction,
including concrete outputs. Definitions without a hook retain the output
declaration's default runtime selection. The correction retains direct scalar
access and records runtime type availability explicitly in compiled node flags
without increasing `CompiledNode` size. Regression tests evaluate concrete
aggregate and callable outputs and typed runtime-signature outputs, check the
selected runtime type, and exercise direct `set()` and `value()`.
Mixed-frame construction and source-row-bound computed-column operations
still need completion before arithmetic nodes can be built.

The owner approved sharing BG's numeric value functions and using its arithmetic
operator objects. The uncommitted extraction is in
`.lanes/library-numeric-values`, branch `agent/library-numeric-values`, based on
main `d05863a6`. `<bg/library/numeric.hpp>` provides inline
`bg::library::numeric::{add, sub, mul, div}` for `i64` and `double`; scalar
operator nodes call the same functions. The existing integer division and
wrapping arithmetic implementations are preserved. The complete `-j12` build
passed; the arithmetic-related CTest filter passed 45/45, and library coverage
validation passed. The combined coverage target reports six tag errors in
unchanged core tests; see that lane's `build/dev/test-coverage.md`. This change
has not been integrated into the frame dependency.

Free expression bundles can be traversed through `operators::getattr`; BG
returns their original child wiring points. Scalar arguments supplied to typed
bundles become const-node points. Direct inspection of their scalar operation
metadata still needs a supported design. Runtime-sized free bundle construction
exists in an installed `details` header; its extension API audience needs review.

Expression and block filter integration, previous-value subscriptions,
strings/symbol cells, compaction, capacity trimming, and delta transport remain
later work. Discuss required BG changes before implementing them. Maintain the
shared storage and operation implementations; do not add a parallel runtime
schema implementation.

For later previous-value support, consumers can request capture directly from
the storage owner reached through `ColumnSource`; no graph requirement
propagation is needed. For expressions with time series scalar parameters,
constructing the expression in a separate node is intended to simplify node
code. Neither feature is implemented by the current foundation.
