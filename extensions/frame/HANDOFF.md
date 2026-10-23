# Frame foundation handoff

Updated: 2026-10-23. The frame lane and current consumer dependencies use main
baseline `5d48e914` plus bounded bitmap search `73622fb7`. The mixed-overload
refactor used main `21a0f87c`.
Earlier frame foundation work was rebased onto main `146ee478`, which included
shared schema metadata (`6419e073`), process-lifetime ownership
of registered type metadata (`160f128b`), compatible generic input-view casts
(`a17a8ffd`), and stateful test-graph literal comparison (`04518839`). Frame
implementation remains on the lane.

The rebase required adapting frame snapshot ownership to main's
`ScalarAllocationPool` API. Commit `cd7b73ad` retains a matching context pool
and preserves direct allocation when the caller supplies an unrelated allocator.
Release Arrow-on verification passed all six suites after this change.

TSF capture supports native and frame view outputs. TSC capture supports native,
view, and computed outputs. Writers register by exact runtime and read through
extension descriptors and borrowed column views. Stateful named row-history
comparison handles TSC and unkeyed TSF; keyed TSF comparison tracks keys by
internal row ID so removal records only need the ID. The shared BG comparison
state is on main through merge `04518839`.

## Work locations

[CONSOLIDATION.md](CONSOLIDATION.md) records the integrated BG changes, archive,
completed lane retirement, and consumer verification.

| Work | Location / branch | State |
| --- | --- | --- |
| BG baseline for current consumer builds | Lane-local Release and Debug installs | `5d48e914` plus bounded bitmap search `73622fb7`; original optimization comparisons held `5d48e914` fixed |
| Bounded bitmap search | `.lanes/index-bounded-search`, `agent/index-bounded-search` | Utility-only commit `de53c9e7`; applied to the frame lane as `73622fb7`; not merged into main |
| Active frame work | `.lanes/frame-field-access`, `agent/frame-field-access` | Frame foundation, expressions, scalar literals, native keyed storage, TSF/TSC input literals, and native/view/computed capture |
| Generic input-view casts | Main | `a17a8ffd`; tested build preserved in `.lanes/generic-input-cast` |
| Shared schema metadata and BG lifetime fix | Main | `6419e073`, `160f128b`; consumer builds use matching lane-local installs |
| Test graph output capture | Main | Integrated through `36eb21fb`, including shared extension comparison dispatch |
| Numeric value extraction | `.lanes/library-numeric-values` | Held; frame does not depend on it |
| Historical frame work | `extensions/frame/archive/` | Proposals, probe sources, and benchmark results preserved; see its README |
| Schema field reflection | Main | Integrated and covered by BG schema reflection tests |

The current frame verification uses these consumer directories:
`build/tsf-row-release-arrow-off` and `build/tsf-row-release-arrow-on` use the
lane-local Release BG install at `dependencies/bg-frame-core-bounded-release-install`;
`build/tsf-row-debug-asan-arrow-on` uses the matching Debug install at
`dependencies/bg-frame-core-bounded-debug-install`. Preserve these generated
dependency installs, the earlier `bg-frame-core-{release,debug}-install` prefixes,
and the existing build caches.

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
- Current cells are nullable `bool`, `i64`, and `double`. All columns are present;
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

The 2026-10-23 optimization verification uses BG baseline `5d48e914` plus bounded
bitmap search `73622fb7` and the current lane-local Release and Debug installs:

| Build | Configuration | Result |
| --- | --- | --- |
| `build/tsf-row-release-arrow-off` | Release, Arrow disabled | Build passed; CTest 5/5 |
| `build/tsf-row-release-arrow-on` | Release, Arrow 24.0.0 | Build passed; CTest 6/6 |
| `build/tsf-row-debug-asan-arrow-on` | Debug, Arrow 24.0.0, ASan/UBSan | Build passed; CTest 6/6 with leak detection; no sanitizer diagnostics |

The sanitizer build instruments the extension and tests, with the matching
uninstrumented Debug BG dependency. Run its CTest through the approved Conda
invocation below; direct sandbox execution prevents LeakSanitizer from running.

```sh
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  conda run --no-capture-output -n hg-build ctest \
  --test-dir build/tsf-row-debug-asan-arrow-on --output-on-failure
```

Scalar-expression cleanup verification used baseline `146ee478`, the lane-local
BG installs, and these lane-root build directories:

| Build | Configuration | Result |
| --- | --- | --- |
| `build/tsf-row-release-arrow-on` | Release, Arrow 24.0.0 | Build passed; CTest 6/6 |
| `build/tsf-row-release-arrow-off` | Release, Arrow disabled | Build passed; CTest 5/5 |
| `build/tsf-row-debug-asan-arrow-on` | Debug, Arrow 24.0.0, ASan/UBSan | Build passed; CTest 6/6 with leak detection; no sanitizer diagnostics |

The sanitizer configuration instruments the frame extension and test targets
with AddressSanitizer and UndefinedBehaviorSanitizer, then links the matching
Debug BG install; BG itself was built without sanitizer compile flags. CTest used
`ASAN_OPTIONS=detect_leaks=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. It ran outside the sandbox
because LeakSanitizer cannot operate under sandbox ptrace.

Mixed binary overload verification used task baseline `21a0f87c`. The scalar-
expression cleanup results above remain historical results at baseline
`146ee478`.

| Build | Configuration | Result |
| --- | --- | --- |
| `build/tsf-row-release-arrow-on` | Release, Arrow 24.0.0 | Build passed; CTest 6/6 |
| `build/tsf-row-release-arrow-off` | Release, Arrow disabled | Build passed; CTest 5/5 |
| `build/tsf-row-debug-asan-arrow-on` | Debug, Arrow 24.0.0, ASan/UBSan | Build passed; CTest 6/6 with leak detection; no sanitizer diagnostics |

Expression regressions cover scalar construction outside graph wiring, scalar
conversion targets, owned scalar captures reused across graphs, mixed operands
in both orders, chained and shared bindings, typed TSB forwarding to filtering,
user names matching internal field names, and parameter invalidation and
revalidation. Existing fusion and concrete intermediate reuse tests pass.

Coverage includes native and runtime schemas, numeric columns, snapshots,
borrowing, Arrow views and copies, projection, and concrete and generic
Row/RowPatch writes. Tests cover schema/type validation before mutation,
nullable cells, all-null insertions, inherited and shadowed fields, generic
type variables, and ownership after graph destruction. Borrowed graph input
tests use native and view TSF runtimes with one concrete schema. They verify
runtime descriptors, source and column buffer identity, nullable cells, and
row and column deltas across invocations. Test checkpoint: `bf4d9483`.

Rebase verification against main `52bac941` rebuilt and installed the Release
BG dependency with `-j12`. Frame compilation exposed the renamed allocation
pool API; after the extension compatibility change in `cd7b73ad`, the complete
Release Arrow-on consumer suite passed 6/6. The rebase itself changed no
extension files. Pool lifetime tests cover snapshots allocated through both
the context allocator and the pool allocator.

TSC literal input verification at `cdd201a2` passed the complete Release Arrow-on
consumer suite, 6/6. Tests cover inferred and explicit runtimes, typed and generic
`Out::apply`, nullable `i64`/`double` appends, empty no-ops, skipped cycles,
invalidation/revalidation, malformed cells, and unsupported output layouts.
Debug and Arrow-off verification have not been repeated for that change.

Prior rebase verification against main `a17a8ffd` rebuilt the Release BG dependency
and Release Arrow-on consumer with `-j12`; the complete consumer suite passed
6/6. Extension files were unchanged by the rebase.

At the current production checkpoint, frame `ad4ad92c` on BG main baseline
`21a0f87c`, root verified the final filter code with lane-local BG installs
matching each consumer configuration below.

| Consumer | Build | Test |
| --- | --- | --- |
| `build/tsf-row-release-arrow-off`, Release, Arrow disabled | Passed | 5/5 suites passed after runtime-stop guards |
| `build/tsf-row-release-arrow-on`, Release, Arrow 24.0.0 enabled | Passed | 6/6 suites passed after runtime-stop guards |
| `build/tsf-row-debug-asan-arrow-on`, Debug, Arrow 24.0.0, ASan/UBSan | Passed | 6/6 suites passed with leak detection enabled; no sanitizer diagnostics |

The benchmark commit adds benchmark code and reports without changing frame
runtime code. Current benchmark builds and measurements are recorded in
[the 2026-10-22 notes](benchmarks/results/2026-10-22/README.md).

The Debug test target was rebuilt after adding scope-bound `stop()` calls to
both runtime graphs in `test_compiled_graph_runtime_storage_is_independent`.
The sanitizer configuration instruments the extension and test targets with
`-fsanitize=address,undefined -fno-omit-frame-pointer` and links the matching
Debug BG install, which was built without sanitizer compile flags. CTest ran
with `ASAN_OPTIONS=detect_leaks=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. LeakSanitizer ran outside
the sandbox because it cannot operate under sandbox ptrace.

The stateful comparison API is integrated on main through merge `04518839`.
At historical frame baseline `b7d52da5`, all six Release Arrow-on
consumer suites pass after the TSC invalidation compatibility update and literal
registration cleanup. GCC reports repeated `-Wstringop-overflow` diagnostics
from the existing `concurrent_buffer_reservation::write_bytes` implementation
in `bg/util/buffer.hpp:444`; the build succeeds.

The generic input-view cast fix passed its focused provider tests (16 assertions
in 2 cases) and the complete Release `bg_tests` binary (2,936,101 assertions in
1,886 cases, including shutdown) in `.lanes/generic-input-cast/build/provider-cast`.
Python was disabled. Root independently reran the new generic-cast test.

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
At that checkpoint, Debug and Arrow-off consumers had not been rerun against
that revision. The current consumer verification above covers both configurations for
the TSF row-literal changes.

Verification against installed main `36eb21fb` rebuilt both extension libraries
and passed all 6 Arrow-on Release suites after the test-only API adaptation in
`72679b4d`. BG testing was disabled for the dependency installation.

Scalar literal and equality verification at `7db3b2f9` on 2026-10-17 rebuilt
the Release Arrow-on consumer with `--parallel 12`. All 6 suites passed;
an independent rerun also passed. Coverage includes nullable native and exact
Arrow literals, runtime schemas, chunk-independent equality, Base/Derived
values and owning copies, malformed input, allocation-failure cleanup, and
ordinary scalar time-series literal inputs and result assertions.
At that checkpoint, Debug, sanitizer, and Arrow-off consumers had not been
rerun for the scalar literal and equality changes. The current consumer verification
above covers those configurations for the TSF row-literal changes.

BG and the extension must use matching build configurations. Debug compiles both
scalar allocation and deallocation checks; Release compiles neither check.

Sanitizer verification found a fixture that destroyed four node templates
before `GraphWiring::finalize()`. The fixture now keeps them in its enclosing
scope. Independent review checked the other new finalization fixtures for
the same lifetime error. All three configurations were rebuilt and their
complete consumer suites passed after the correction.

The sanitizer test used ASAN_OPTIONS=detect_leaks=0 and
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 at an earlier sandbox-limited
checkpoint. The current Debug Arrow-on verification enables leak detection and
runs outside the sandbox, as recorded below. The sanitizer configuration
instruments the frame extension and test targets and links the matching Debug
BG install, which was built without sanitizer compile flags. Clang and MSVC
have not been tested.

Current CTest results are in each consumer's `Testing/Temporary/LastTest.log`.
Earlier verification logs remain preserved. The Arrow-on Release shared-library
RPATH uses the installed prefix and Conda libraries.

Current consumer builds and tests use the lane-root `tsf-row` directories:

```sh
conda run --no-capture-output -n hg-build cmake --build \
  build/tsf-row-release-arrow-on --parallel 12
conda run --no-capture-output -n hg-build ctest \
  --test-dir build/tsf-row-release-arrow-on --output-on-failure
conda run --no-capture-output -n hg-build cmake --build \
  build/tsf-row-release-arrow-off --parallel 12
conda run --no-capture-output -n hg-build ctest \
  --test-dir build/tsf-row-release-arrow-off --output-on-failure
conda run --no-capture-output -n hg-build cmake --build \
  build/tsf-row-debug-asan-arrow-on --parallel 12
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  conda run --no-capture-output -n hg-build ctest \
  --test-dir build/tsf-row-debug-asan-arrow-on --output-on-failure
```

Run the sanitizer test command outside the sandbox because LeakSanitizer cannot
operate under sandbox ptrace restrictions.

## Shared column expression implementation

Committed prerequisites are `5b846d94` (expression scalar and TSB schema),
`f384f1eb` (n-ary aligned batches), and `1bc4569e` (batch subspan).
`src/arithmetic.cpp` provides arithmetic and conversion registrations, and
`src/column_expression.cpp` provides shared compilation and execution. The
extension links `bg::library` and registers matching `i64` and `double`
arithmetic through BG operators. Mixed integer/double operands fail during
wiring unless explicitly converted. Identity conversion retains the source
wiring point; cross-type conversion uses a mapped BG operator.

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

`Expression` is an immutable scalar describing operations, unresolved names,
and owned scalar values. `col(name)` identifies a column; `param(name)`
identifies a named argument. Construction requires no graph or wiring context.
Conversion records its target scalar type. Scalar expressions contain no
wiring points, wiring templates, or type maps.

Combining a scalar expression with a wiring point invokes a registered BG
operator overload. Its ordinary TSB result holds the constant expression and
explicit time series bindings. Internal binding references are separate from
user column and parameter names. Chaining graph operations preserves these
dependencies without rebuilding expression trees during evaluation.

`bind_expression(expr, named_arguments...)` resolves names from its arguments.
`bind_expression(frame, expr, named_arguments...)` also resolves column names
from the frame. The expression argument can be a scalar expression or a TSB
carrying its bindings. Binding invokes BG operators to select typed block
functions and returns their deferred column wiring point. Each binding resolves
types independently. Runtime schemas are already resolved during wiring.
Numeric conversion remains explicit. Graph conversion uses
`operators::convert[type<TSC<T>>](column)`; scalar expression conversion uses
`frame::convert<T>(expression)`.

Binding resolves scalar operations during graph construction. The column
expression TSB is the resolved materialisation form of a BG operation. It retains
declared upstream dependencies. Block evaluation and concrete intermediate reuse
use the existing expression compilation path. Graph authors use operators and
binding functions to construct these bundles.

Arithmetic supports matching `i64` or `double` columns, constants, and captured
`TS<T>` values. Constants use the existing `operators::const_` path. Scalar
convenience graphs declare their `TSC<T>` result type. Overload requirements
require a column operand; ordinary BG scalar arithmetic keeps its existing
overloads.

Concrete arithmetic, comparison, and logical binary graphs declare disjoint
column/any and `TS<T>`/column signatures. Their requirements check original
operand types where a deferred Union can also offer a generic TSB form. Mixed
expression overloads use named aggregate registrations and separate left and
right binding variables. Tests verify independent `TS<i64>` and `TS<double>`
captures, native `TS<i64>` addition, and rejection of Boolean/numeric addition.

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

Boolean storage is implemented across columns, TSC/TSF, literals, tick capture,
computed outputs, and Arrow import/export. Matching `i64` and `double` column
comparisons and expression-based filtering use the shared expression compiler
and block evaluation path. Matching `i64` and `double` columns now support
addition, subtraction, multiplication, and division; division returns `double`.
Explicit column conversion uses the existing mapped BG `operators::convert`
path. Identity conversion retains its source wiring point. Numeric conversion
supports `i64` to `double` and `double` to `i64`.

The [shared block-operation contract](block_operation_proposal.md) and
[computation nodes and input bindings](expression_node_proposal.md) are
accepted and implemented. Production arithmetic uses the shared expression
compiler for matching `i64` and `double` operands. Integer addition,
subtraction, and multiplication wrap modulo 2^64. Division converts both
operands to `double`; a valid zero divisor throws `std::domain_error`. Null
operands produce null without evaluating the operation. `double` to `i64`
conversion truncates toward zero, throws `std::domain_error` for NaN, and
throws `std::overflow_error` outside the `i64` range.
Tests exercise unary callbacks with a different result type, heterogeneous
column inputs, and captured scalar types. The mixed-type graph verifies one
fused computation node and one `Bundle<{i64, double}>` parameter snapshot.
Scalar-only column expressions fail during wiring because they have no row
domain. Subtraction uses the shared factory and the same callback adapter as
addition; it adds no compiler or evaluator behavior. Boolean values use packed
storage with separate validity. The expression callback test covers a Boolean
result. All six comparison operators are registered for `i64` and `double`;
they return nullable `TSC<bool>` and propagate null operands. Expression-based
filtering uses the ordinary `filter_` operator with a graph-only predicate
union and a `TSF` view output.
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
Frame registers `TimeSeriesTickOperations` writers by exact runtime, with
discard and `testing::TickLiteralComparison` handlers registered by resolved
semantic series type. Capture does not require replay. The framework handles
invalidation and skipped ticks before calling a comparison handler; successful
comparison consumes one valid payload without changing its encoded values. See
[BG testing](../../docs/testing.md).

Frame writers visit delta row sets through extension descriptors and borrowed
column views. Each TSF tick stores a pointer to its actual registered schema so
comparison and discard can decode Derived fields through Base annotations and
references. Payload cells are copied and contain no borrowed cell pointers.
Keyed removals record only the internal row ID. The comparison state remembers
keys from additions and uses that mapping to match later removals. This permits
comparison and discard after graph destruction, including capture under an
unkeyed Base annotation. Expected keyed deltas match by key, independent of row
order, with exact patch field sets and omitted addition fields interpreted as
null. See
[keyed capture](README.md#keyed-tsf-result-capture).

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
Time-series row updates use [key-addressed row literals](README.md#tsf-row-updates).
Insertions must allocate internal row IDs; patch and removal operations use
existing IDs. Scalar literal handlers register automatically through canonical
type factories. Exact Arrow runtime handlers construct Arrow storage using the
same native parser. Malformed names, cell types, and column lengths fail during
construction. See [scalar literal syntax](README.md#scalar-literals-and-equality).

## Named row-history capture

The frame extension adds TSC and unkeyed TSF tick writers and stateful result
comparators for native, view, and computed outputs. Their payloads retain
internal row IDs for each valid tick; the comparison state joins add, modify,
remove, and invalidation events into row histories across one `equals()` call.
TSC history values support `i64` and `double`. Unkeyed TSF history values use
the actual runtime schema and preserve the sparse field set for each patch.
Keyed TSF comparison uses its row-ID-to-key history for removals.

Expected names identify one row lifetime within an `equals()` call. Names stay
reserved after removal or invalidation. Different names may match actual rows
with equal complete histories. Reused internal IDs start a new actual history.
The tests use named expectations and cover row-ID reuse, invalidation, nullable
TSC cells, nullable pass-through and computed cells, filtering membership,
source-domain switches, identical histories, complete-history matching, exact
TSF patch fields, name reuse rejection, empty published deltas, and skipped
ticks.

At historical main baseline `b7d52da5`, all six Release Arrow-on consumer
suites passed after the stateful comparison merge `04518839`. The verified
frame implementation is recorded on the active frame lane.

## Performance artifacts

Current paired Release measurements are recorded in
[the 2026-10-23 optimization results](benchmarks/results/2026-10-23/README.md).
The report preserves three runs per version for all 30 cases, process-median
ranges, runtime library hashes and loading commands, and complete timing data.
Both versions use the same fixed BG dependency. Preserve the baseline runtime
copies under `build/frame-optimisation-2026-10-22/before-runtime`.
The [bounded validity follow-up](benchmarks/results/2026-10-23/bounded-search/README.md)
compares `2f180034` with `3b59d9d7` using three further paired Arrow runs per
version. Dense borrowed export plus sum improves from 0.220 to 0.032 ms;
scattered selection still pays for 49,168 chunks. The measured earlier
optimized runtimes are preserved under
`build/frame-optimisation-2026-10-22/pre-bounded-runtime`.

Earlier matching Release consumer measurements are recorded in
[the 2026-10-22 benchmark notes](benchmarks/results/2026-10-22/README.md), with
separate [expression](benchmarks/results/2026-10-22/expression-release-arrow-off.md)
and [Arrow](benchmarks/results/2026-10-22/arrow-release-arrow-on.md) reports.
The dated notes record the source revision, BG baseline, compiler and host,
commands, measured ranges, and limits. The older archived sample below used a
Release extension with Debug BG and is not directly comparable.

Subsequent [Callgrind profiles](benchmarks/results/2026-10-22/profiling.md) cover
all 30 cases using the same binaries. Raw dumps and parsed analyses remain in
`build/frame-profile-2026-10-22`; the dated results contain compact instruction
counts. Profiles identify row-by-row batch construction, redundant full predicate
traversal, selected-order rebuilds, and fragmented Arrow allocation as the main
costs. These profiles predate the implemented optimizations below.

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

BG test-graph output capture, frame scalar literals and content equality,
TSF row-literal updates, TSC append literals, and keyed TSF result capture and
delta comparison are implemented. Named TSC and unkeyed TSF row-history
comparison is implemented and verified for native, view, and computed outputs.

Unkeyed TSF and TSC input literals append rows. Nodes can also modify and remove
rows through their internal IDs, so output deltas can contain all three
operations.

Boolean storage, Arrow support, numeric comparisons, and expression-based
filtering are implemented.

Filter optimization now avoids rebuilding unchanged selection on full predicate
reevaluation. Selected IDs reuse initialized storage and supply callback-scoped
spans with optional consecutive-slot mappings. Native and aligned consumers use
matching mappings directly and retain existing lookup or gather behavior for
other mappings. Arrow borrowed export allocates validity only when a batch has
nulls. Native validity searches inspect bitmap words within the batch bounds.
Node APIs and borrowed versus owning export contracts are unchanged.
See the [row traversal contract](README.md#values-and-rows) and
[selection metadata decision](docs/decisions/selected-row-span-metadata.md).
Compaction, capacity trimming, and reducing fragmented Arrow object overhead
remain later work.

The Boolean foundation uses packed value bits with separate nullable validity.
It covers `Column<bool>`, frame fields, TSC/TSF storage and deltas, literals,
computed outputs, and the expression callback result path. Arrow import handles
sliced Boolean arrays by adjusting the byte pointer and retaining the residual
bit offset. Borrowed export retains the packed buffer; gather-copy export packs
one dense chunk. Release Arrow-on build and all six frame CTest suites passed
on this lane. The Release Arrow-off consumer passed all five suites. The Debug
ASan/UBSan consumer passed all six suites with leak detection enabled. The
sanitizer configuration instruments the extension and test targets and links
the matching Debug BG install; BG itself was built without sanitizer compile
flags.

Fixed-name `Out<TSF<Schema>, frame::computed<"mid">>` and runtime-selected
`Out<TSF<Schema>, frame::computed<>>` outputs use the shared runtime-schema
view backend. Runtime-selected names come from an output runtime type hook;
without one, wiring rejects the declaration. Outputs accept writes by name or
schema ordinal. Dedicated Arrow-on tests cover
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

Standalone computed TSC outputs use the approved `computed<>` declaration.
Frame division returns `double` and uses the shared column block callback path.

The two BG changes integrated in this task are scalar copy runtime selection
and wiring-time output runtime selection. The frame extension uses scalar copy
selection for owning frame copies and the output hook to select frame layouts.
Frame output layout options remain extension-owned.

The shared numeric function extraction remains held. The arithmetic,
conversion, and comparison implementations use the frame extension's existing
column expression path. Expression-based filtering is described in
[filter_proposal.md](filter_proposal.md).

## Key metadata and native keyed storage

The active frame lane uses main's shared schema metadata and process-lifetime
ownership of registered type metadata.
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
validate key uniqueness with a temporary hash index of row positions. The
validator records batch starts for collision checks; it does not add persistent
key-index state to scalar frames. TSF row literal updates use key columns for
insert, patch, and removal as documented in the [extension README](README.md#tsf-row-updates).
Keyed TSF view outputs preserve the source's complete ordered key tuple. A
projection can rename key fields, but target key fields must map to the source
key fields in key order. Binding rejects unkeyed sources, incomplete or
reordered mappings, and other source fields before changing output state.
Unkeyed outputs do not constrain the source key mapping. Computed TSF outputs
cannot own key fields. See the [view contract](README.md#tsf-inputoutput-surface).
Capture serializes row IDs for removals and uses comparison-owned key history to
match keyed removal literals.

Tests cover allocation rollback for keyed insertion and removal-key retention,
including retry after failure. Scalar validation tests cover duplicate single
and composite keys, including an all-null tuple and runtime-schema construction.
Arrow validation tests cover duplicate keys across chunks, a valid composite
key import with unequal chunk boundaries and a sliced nullable array, and a
collision chain with a distinct key before a duplicate. Current consumer
configurations and results are in [Verification](#verification).

Keyed-view tests cover renamed single and composite key fields, partial and
reordered key mappings, keyed outputs from unkeyed sources, unkeyed projections
from keyed sources, filtering, runtime schema cache refresh, computed key
rejection, and rejection before invalidating a previously bound output.
The Release Arrow-on build and all six consumer test suites pass for this change.

Registered types, canonical runtime descriptors, and schema metadata follow
BG's [process-lifetime ownership rule](../../docs/decisions/0071-process-lifetime-type-storage.md).
Ordinary node and scalar values keep normal destruction. The frame lane uses
`bg::util::static_storage<T>` for its canonical runtime and layout cache owner,
typed column runtime statics, FrameView scalar and mixed-view runtimes, and
Arrow runtime cache owner.

Previous-value subscriptions, strings and symbols, compaction, capacity
trimming, and delta transport remain later work. Previous-cell capture, if
needed, is requested from the owning storage through ColumnSource; graph
back-propagation is not required. These features are not implemented.
