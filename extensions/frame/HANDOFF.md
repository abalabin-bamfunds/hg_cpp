# Frame foundation handoff

Updated: 2026-10-19. The active frame lane is based on main `2ad4747c`, with
shared schema metadata `8ab0b121` and BG process-lifetime storage fix
`97f37b55` cherry-picked.
Expressions are committed as `30e6fa80`; scalar Column/Frame literals and
content equality are committed as `02fc8c17`. Native keyed storage and its
tests are committed on this lane. Raw scalar and Arrow duplicate-key validation
is the next separate change; see
[Key metadata and native keyed storage](#key-metadata-and-native-keyed-storage).
TSF/TSC literals, delta comparison, and capture remain outstanding.

## Work locations

[CONSOLIDATION.md](CONSOLIDATION.md) records the integrated BG changes, archive,
completed lane retirement, and consumer verification.

| Work | Location / branch | State |
| --- | --- | --- |
| Main | `/home/abalabin/_bg`, `main` | `2ad4747c`; shared schema metadata and frame work are unmerged |
| Active frame work | `.lanes/frame-field-access`, `agent/frame-field-access` | Native keyed storage is committed; frame work remains unmerged |
| Shared schema metadata | `.lanes/schema-metadata`, `agent/schema-metadata` | `cb0d861b`; process-lifetime storage fix passes Release and Debug consumer checks |
| Test graph output capture | Main | Integrated through `36eb21fb`, including shared extension comparison dispatch |
| Numeric value extraction | `.lanes/library-numeric-values` | Held; frame does not depend on it |
| Historical frame work | `extensions/frame/archive/` | Proposals, probe sources, and benchmark results preserved; see its README |
| Schema field reflection | Main | Integrated and covered by BG schema reflection tests |

Current keyed-storage consumer builds use BG from
`/home/abalabin/_bg/.lanes/schema-metadata/build/install/release` for Release
(Arrow on and off) and
`/home/abalabin/_bg/.lanes/schema-metadata/build/install/debug` for Debug
(Arrow on). Older BG installs and dependency caches under
`extensions/frame/build/dependencies/` remain on disk but are not used for this
verification. Conda supplies Arrow and other unrelated dependencies.

The Debug dependency build cache is `dependencies/bg-debug`; it is generated
and untracked. The Release cache is
`extensions/frame/build/dependencies/bg-release`. Preserve both caches.

Preserve the existing recovery stashes:

```text
6c9a333b5b819b453026f02beee198a7a32c4946
16cbdc77d65d43478e17eccff1f5930dca630415
f650a30a79bd707b8f566242c7bbe2d48020e578
```

The latest 2026-10-16 rebase applied without conflicts and restored the
uncommitted frame changes. The tracked patch and four untracked source files
matched their saved hashes after restoration. The temporary rebase stash was
removed; all preexisting stashes remain unchanged. Recovery copies are under
`/home/abalabin/_bg/build/frame-lane-recovery/2026-10-16-215427-rebase`.

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
1,720/1,720. Earlier verification built Debug and Release BG dependencies from
`19403a4d`, with testing, Python, and benchmarks disabled. The complete BG suite
is not repeated for extension verification.

All three consumer caches resolve BG, ddvisitor, and tpack from the matching
installed prefix. Conda supplies unrelated dependencies including Arrow 24.0.0. Builds
regenerated their CMake configuration and CTest metadata as needed.

Baseline consumer verification against installed main `31584c4c`, before
expression construction and scalar broadcasts:

| Consumer | Build | Test |
| --- | --- | --- |
| build/field-access-on, Release, Arrow enabled | Passed with -j12 | 6/6 suites passed |
| build/cc4-checkpoint-off, Release, Arrow disabled | Passed with -j12 | 5/5 suites passed |
| build/cc4-lifetime, Debug, Arrow enabled, ASan/UBSan | Passed with -j12 | 6/6 suites passed; no sanitizer diagnostics |

Expression and broadcast Debug ASan/UBSan verification against installed main
`1a5ff458` passed 6/6 suites. Final verification against installed main
`19403a4d` on 2026-10-15 covers the shared expression factory, compiler and
computation node, heterogeneous column alignment and parameter snapshots,
stored filter row order, BG-managed parameter validity, and double-column
subtraction with fused addition/subtraction callbacks:

| Consumer | Build | Test |
| --- | --- | --- |
| build/cc4-lifetime, Debug, Arrow enabled, ASan/UBSan | Passed with -j12 | 6/6 suites passed; no sanitizer diagnostics |
| build/field-access-on, Release, Arrow enabled | Passed with -j12 | 6/6 suites passed |
| build/cc4-checkpoint-off, Release, Arrow disabled | Passed with -j12 | 5/5 suites passed |

Rebase verification against installed main `359e1aee` on 2026-10-16 rebuilt BG
and the Release Arrow-on consumer with `--parallel 12`; all 6 suites passed.
The deferred-column Union matching regression was recovered from
`.lanes/frame-source-matching-check` into `tests/expression_test.cpp` and passed.
That checker lane has no other unique implementation changes.
The frame expression helpers now use public `bg::bundle(span<const WiringArgument>)`.
Explicit target bundle schemas retain the previous field dereferencing.
Debug and Arrow-off consumers have not been rerun against this revision.

Verification against installed main `36eb21fb` rebuilt both extension libraries
and passed all 6 Arrow-on Release suites after the test-only API adaptation in
`72679b4d`. BG testing was disabled for the dependency installation.

Scalar literal and equality verification at `7db3b2f9` on 2026-10-17 rebuilt
the Release Arrow-on consumer with `--parallel 12`. All 6 suites passed;
an independent rerun also passed. Coverage includes nullable native and exact
Arrow literals, runtime schemas, chunk-independent equality, Base/Derived
values and owning copies, malformed input, allocation-failure cleanup, and
ordinary scalar time-series literal inputs and result assertions.
Debug, sanitizer, and Arrow-off consumers have not been rerun for this change.

BG and the extension must use matching build configurations. Debug compiles both
scalar allocation and deallocation checks; Release compiles neither check.

Sanitizer verification found a fixture that destroyed four node templates
before `GraphWiring::finalize()`. The fixture now keeps them in its enclosing
scope. Independent review checked the other new finalization fixtures for
the same lifetime error. All three configurations were rebuilt and their
complete consumer suites passed after the correction.

The sanitizer test used ASAN_OPTIONS=detect_leaks=0 and
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1. Leak detection remains disabled because of
environment ptrace restrictions. The sanitizer frame build links the Debug BG
dependency. Clang and MSVC have not been tested.

Current CTest results are in each consumer's `Testing/Temporary/LastTest.log`.
Earlier verification logs remain preserved. The Arrow-on Release shared-library
RPATH uses the installed prefix and Conda libraries.

Run builds and tests from the active lane:

```sh
conda run --no-capture-output -n bg-build cmake --build extensions/frame/build/field-access-on --parallel 12
conda run --no-capture-output -n bg-build ctest --test-dir extensions/frame/build/field-access-on --output-on-failure
```

Use `cc4-checkpoint-off` for Arrow-off Release. For `cc4-lifetime`, prefix the
CTest command with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`.

## Shared column expression implementation

Committed prerequisites are `5b846d94` (expression scalar and TSB schema),
`f384f1eb` (n-ary aligned batches), and `1bc4569e` (batch subspan).
`src/arithmetic.cpp` provides arithmetic callbacks and registrations,
and `src/column_expression.cpp` provides shared compilation and execution. The extension links
`bg::library` and registers double-column addition and subtraction on
`operators::add` and `operators::sub`. It uses exact cell types; mixed
integer/double operands fail during wiring. Explicit column conversion is
future work. Preserve the held numeric extraction.

The expression option has cost zero; the computed TSC option has cost one.
`make_column_expression` creates the expression bundle and concrete recipe
from a typed callback and operand wiring points. The callback has independently
typed unary or binary operands and result. `ExpressionCompiler` recognises
the TSB/callback contract, produces a `CompiledExpression`, and passes
deduplicated concrete leaves through an ordinary TSB. The runtime evaluates
aligned blocks, reuses 128-row scratch, and writes
`Out<TSC<RESULT>, computed<>>`. Repeated
children reuse instruction results. A forced concrete intermediate stops expression
descent, including when the concrete demand is recorded after expression-form
selection. Independently constructed equal expressions need not share a node.

`ColumnExpressionNode<RESULT>` coordinates input preparation, row selection, and result
publication. `CompiledExpression::for_each_result_batch` owns block evaluation and
reuses scratch buffers across batches. `ExpressionCompiler` appends the result instruction last.
Binding state is allocated in `start()` and released in `stop()`.
Wiring assigns each instruction an aligned result-buffer offset from its
callback's size and alignment. Evaluation reuses these buffers for 128-row
blocks. Callbacks set validity for every output position and read only valid
input cells.

Selected expression wrappers retain the original deferred TSC source identity.
The expression builder normalizes operands to that source before constructing
its TSC fields. `ExpressionCompiler` distinguishes ordinary concrete columns from deferred
points with a concrete recipe. Arithmetic requires a shared live RowSet
descriptor and row domain. Projection and computed outputs retain their source
selection; filters own distinct selections. Equal row contents do not make
independent selections compatible. Physical mappings may differ. Input storage
replacement, lost delta continuity, parameter updates, and revalidation rebuild
values.

Filter outputs maintain their selected row sequence in source order. The basic
implementation rebuilds that sequence upstream when membership changes or the
source requires a rebuild. `util::contiguous_bunch<row_id_t>` stores one ID per
selected row. Payload updates that preserve membership retain the sequence.
Downstream traversal uses the stored sequence. Previous-membership tracking
remains separate from order storage. Selection storage and update optimisations
require measurements before further changes.

Tests cover chained and repeated expressions across 257 rows, validity around
128-row scratch boundaries, sparse updates, null and all-null rows, removal and
ID reuse, invalidation/revalidation, projection, incompatible
domains/selections, fusion, and late concrete demands. The late-demand fixture
combines computed and native columns with distinct physical mappings and
covers both additions and an existing-row update. Its aggregate node template
lives in the enclosing test scope through graph compilation, as required by
[BG's wiring contract](../../docs/graph-wiring.md#wiring-template-call).
The filter-order test uses the actual output binder and runtime input storage.
It verifies source order `[2, 1]` after slot reuse and verifies that repeated
enumeration and positional reads do not traverse the source.

Independent read-only review found no confirmed defects in deferred TSC
materialisation, binding, capture ownership, or parameter updates. Current
consumer results are recorded in [Verification](#verification).
The subtraction review found no correctness blockers in callback dispatch,
operand order, overload selection, or the expression syntax.

### Expression construction and binding

The approved graph-code API uses one `Expression` representation for operations,
unresolved names, and attached arguments. `col(name)` identifies a column;
`param(name)` identifies a named argument. Captures retain owned `WiringArgument`
values. Expressions can be constructed before a frame or its schema is known.

`bind_expression(expr, named_arguments...)` resolves names from its arguments.
`bind_expression(frame, expr, named_arguments...)` also resolves column names
from the frame. Binding invokes the existing BG operators and returns their
deferred wiring point. Each binding resolves types independently. Runtime
schemas are already resolved during wiring. Numeric conversion remains explicit.

The syntax tree exists only during graph construction. The internal expression
TSB is the resolved materialisation form of a BG operation. It retains declared
upstream dependencies. Block evaluation and concrete intermediate reuse use the
existing expression compilation path. Manually constructed expression bundles are not the
graph-author API.

Addition and subtraction support double columns, double constants, and captured `TS<double>`
values. Constants use the existing `operators::const_` path. Scalar convenience
graphs declare their `TSC<double>` return type. Overload requirements require a
column operand; ordinary BG scalar arithmetic keeps its existing overloads.

`ExpressionCompiler` collects distinct captured time series into an ordinary
TSB. A separate parameter-update node publishes their current values as
`TS<Bundle<PARAMETERS>>`, preserving each scalar type;
parameter ticks do not change expression structure. Pure column expressions do
not create this node. Block computation reads the snapshot once per invocation.
The parameter-update node declares `all_valid`; the computed-column node requires
its snapshot input. BG applies input validity and output invalidation through the
[normal node contract](../../docs/node-api.md#required-input-invalidation).
Each operand is a column batch or scalar broadcast; broadcasts do not allocate
column storage. Column changes recompute affected rows; parameter changes
recompute all current rows. Invalid parameters invalidate the result;
revalidation rebuilds it. Compiled expressions retain instructions and operand
indices, without wiring points.
Parameter validity coverage includes an invalid item while another parameter
keeps the input collection root valid.

BG free TSBs reject direct borrowed graph inputs. `ExpressionCompiler` uses these collections
for both column leaves and parameters, so direct `GraphWiring::borrowed_input`
bindings remain unsupported here. Deduplication topology tests use ordinary
source nodes.

Comparisons, boolean cell storage, and explicit column conversion
follow this foundation.

The [shared block-operation contract](block_operation_proposal.md) and
[computation nodes and input bindings](expression_node_proposal.md) are
accepted and implemented. Production arithmetic supports double addition and subtraction.
Tests exercise unary callbacks with a different result type, heterogeneous
column inputs, and captured scalar types. The mixed-type graph verifies one
fused computation node and one `Bundle<{i64, double}>` parameter snapshot.
Scalar-only column expressions fail during wiring because they have no row
domain. Subtraction uses the shared factory and the same callback adapter as
addition; it adds no compiler or evaluator behavior. Boolean physical layout
and expression-based filtering require design review before implementation.
Subtraction tests cover column, constant, and captured-time-series operand
order, nulls around block boundaries, sparse row updates, and parameter ticks.
Mixed addition/subtraction tests verify callback values, fusion, and forced
intermediate reuse.

## Literal support

BG main includes the literal extension APIs through merge `8fb5bcc0`, including
the sentinel changes through `ee61d6d6`. The standard `test_graph` literal source
calls erased `Out::apply`.

`ScalarLiteralOperations::construct` consumes `ScalarLiteral` into uninitialized
scalar storage, and `TimeSeriesLiteralOperations::apply` consumes
`TimeSeriesLiteral` through `OutputState`. Their existing `ScalarDispatch` and
`TimeSeriesDispatch` registries prefer exact runtime registration, then semantic
registration. These are extension-facing APIs. Nodes use
`ScalarValue::from_literal()` and `Out::apply()`. Frame scalar literal handlers
share one implementation across concrete and runtime schemas.

`ScalarLiteral` and `TimeSeriesLiteral` are aliases of the same borrowed
recursive carrier. Scalar time series replacements pass that carrier directly
to scalar construction. Leaves use ordinary scalar input construction;
provider sequence handlers receive the original literal without tree copying.

`literal::Sentinel` is noncopyable. Equality compares singleton addresses; its
qualified symbol name is diagnostic. BG exports `SKIP`, `INVALID`, `REMOVE`, and
`REMOVED` once from its library. Extensions export their own singleton instances
with static storage duration. Scalar storage retains and copies a singleton
pointer; typed reads return `const Sentinel&`.

Calling `REMOVED(value)` produces a separate `literal::TaggedLiteral` scalar
that borrows the sentinel and `ScalarLiteral` argument. Both scalar types have
canonical semantic types and runtimes. Extension sentinel instances require no
additional registration. Each consumer interprets its supported instructions.
Tagged literal recognition constructs its result inside a void visitor because
DDV currently assigns visitor results, which reference members cannot support.

Verification passed 1,821 core test cases and 144 library cases. The latest
focused suite passed 15 cases and 70 assertions, covering literal identity,
collection updates, provider marker handling, and queue cleanup.

Main `36eb21fb` captures provider-defined time series through tick serialization.
Frame must register `TimeSeriesTickOperations` write and discard handlers, and
`testing::TickLiteralComparison` by canonical semantic series type. Capture does
not require replay. The framework handles invalidation and skipped ticks before
calling a comparison handler; successful comparison consumes one valid payload
without changing its encoded values. See [BG testing](../../docs/testing.md).
Frame has not registered these handlers yet.

Scalar `TS<Frame<...>>` and `TS<Column<...>>` use scalar capture and comparison.
The capture buffer's generic fallback retains an owning `ScalarValue<>`;
specialized `ScalarBufferOperations` are unnecessary for test capture. Column
and Frame content equality compares nulls and ordered cells across native and
Arrow layouts. Frame equality also compares the actual schema and row count,
including Derived fields observed through a Base annotation. Numeric cells use
ordinary numeric equality, so NaN compares unequal. TSF/TSC delta comparison
uses the separate time-series handler.

The approved scalar literal format uses a sequence of nullable cells for a
Column and named column sequences for a Frame. Null cells use the exported
frame-extension sentinel `frame::NULL_VALUE`. Frame columns have equal lengths.
Current literal leaves do not support `std::nullopt` as a null cell.
Time-series row-update syntax remains undecided.
Insertions must allocate internal row IDs; patch and removal operations use
existing IDs. Scalar literal handlers register automatically through canonical
type factories. Exact Arrow runtime handlers construct Arrow storage using the
same native parser. Malformed names, cell types, and column lengths fail during
construction. See [scalar literal syntax](README.md#scalar-literals-and-equality).

## Performance artifacts

The preserved active-lane sample is archived at:

```text
extensions/frame/archive/benchmarks/foundation-benchmark/shared-view.md
extensions/frame/archive/benchmarks/foundation-benchmark/shared-view.csv
```

The workload starts with 100,000 rows and runs source mutation, filtering,
selection, and sum. Allocation and destruction are included. This historical
sample used a Release extension with Debug BG. Rerun it with matching
configurations before using its timings as performance evidence.

The earlier schema-hooks comparison and direct sparse-bunch PyArrow probe are
preserved under `extensions/frame/archive/`. Their source and measurement
conditions are documented in the archive README. These results inform the
design; they do not replace the consumer verification recorded above.

## Status and remaining work

BG test-graph output capture and frame scalar literals and content equality are
implemented. TSF/TSC result-capture handlers remain outstanding. Agree row-update
literal syntax before implementing time-series literal application and delta
comparison through the existing extension APIs.

Boolean storage, comparisons, and expression-based filtering are the next
frame features and require design review before implementation.

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

The shared numeric function extraction remains held. The arithmetic implementation is
described above. Other numeric operators, explicit column conversion, and
expression-based filtering remain later work.

## Key metadata and native keyed storage

The active frame lane includes `8ab0b121` (`Add shared schema metadata`) and
the BG process-lifetime storage fix `97f37b55`.
`bg::frame::key_metadata<...>()` records an ordered list of key column names in
schema metadata under `bg.frame.key`; concrete and runtime schemas use the same
representation. Key fields must have `i64` cell types; `double` remains
supported for non-key columns. Schema validation checks key names and types across inherited
schemas, rejects changes to inherited keys, and requires a keyed parameter to
match the argument's key metadata. An unkeyed parameter can match an otherwise
compatible keyed schema.

Native `TSF` storage keeps a live key-to-row-ID index. It rejects duplicate
live keys and changes to key columns. Null key cells are supported, including
in composite keys. Removal retains the removed key tuple through the removal
cycle. If the key is re-added during that cycle, the new live row gets a new
row ID while the removed row's key and ID remain available in the delta.
`find_key()` and `removed_key()` are storage APIs, not node-facing APIs.

`FrameView(columns, size, source)` is an extension-facing trusted binding
constructor. Its caller must provide columns from storage whose schema and key
invariants are already established. Raw scalar construction and Arrow import
do not yet validate key uniqueness. Node-facing key lookup, keyed view behavior,
and TSF/TSC row literal and capture behavior remain open.

The native keyed-storage test target builds in Release with Arrow disabled:

```sh
conda run --no-capture-output -n bg-build cmake --build build/cc4-checkpoint-off \
  --target bg_frame_frame_test -j12
```

Focused tests cover allocation rollback for keyed insertion and removal-key
retention, including retry after failure. The Arrow-off Release suite passes
5/5 tests in `build/cc4-checkpoint-off`. The Arrow-on Release suite passes
6/6 tests in `build/field-access-on`. Both use BG from
`/home/abalabin/_bg/.lanes/schema-metadata/build/install/release`.

The matching Arrow-on Debug/ASan build in `build/cc4-lifetime` passes all 6
tests with no ASan or UBSan diagnostics. It uses BG from
`/home/abalabin/_bg/.lanes/schema-metadata/build/install/debug`. The build uses
`-fsanitize=address,undefined -fno-omit-frame-pointer`; CTest ran with
`ASAN_OPTIONS=detect_leaks=0` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.

The earlier shutdown abort occurred when `SchemaMetadata` released a
`ScalarValue<List<string>>` after its runtime type had been destroyed. GDB
showed `__cxa_pure_virtual` from `bg::details::release_scalar_value` while
`TypeRegistry` destroyed a schema. The user chose process-lifetime ownership
for registered types, canonical runtime descriptors, and schema metadata;
ordinary node and scalar values keep normal destruction. The frame lane uses
`bg::util::static_storage<T>` for its canonical runtime and layout cache owner,
typed column runtime statics, FrameView scalar and mixed-view runtimes, and
Arrow runtime cache owner. The updated Release Arrow-on and Arrow-off consumer
processes exit cleanly, and all 11 tests pass. The matching Debug/ASan consumer
process also exits cleanly; all 6 tests pass without sanitizer diagnostics.

Previous-value subscriptions, strings and symbols, compaction, capacity
trimming, and delta transport remain later work. Previous-cell capture, if
needed, is requested from the owning storage through ColumnSource; graph
back-propagation is not required. These features are not implemented.
