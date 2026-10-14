# BG frame extension

`bg_frame` is a separate BG extension. It adds nullable numeric columns,
frames with BG schemas, and native incremental column storage through BG's
public type, node, and runtime extension points.

- `bg::frame::Column<bg::i64>` is an immutable owning scalar.
- `Frame<Schema>` is an immutable owning scalar with named columns.
- `ColumnView<T>` and `FrameView<Schema>` are borrowed scalar read views,
  selected by `scalar_storage_traits`.
- `bg::frame::TSC<T>` and `TSF<Schema>` are incremental time series. They share
  row identity and report current-cycle row changes. `TSF` exposes fixed-schema
  columns as child `TSC` values.

The current cell types are `i64` and `double`. `TSC` and `TSF` use ordinary
outputs by default. Both support borrowed views; `TSC` and `TSF` also support
computed outputs. `TSF` supports borrowed projections and row filters. The
optional Arrow adapter supports numeric scalar imports and exports.
Previous-cell subscriptions and expression kernels are later work.

`frame::view` is an output option owned by this extension. Use it with
`Out<TSC<T>, frame::view>` and `Out<TSF<Schema>, frame::view>`. BG does not
interpret the option, and built-in time series outputs do not support it.

Guarantee variables use BG's type matching and resolution. Unsupported
resolved guarantees are rejected explicitly. Provider initialization registers
the default runtime with BG.

## Values and rows

Row IDs are internal 32-bit identities within one row domain. They are
independent of user-defined key or index columns. A live row keeps its ID.
Removal reserves that ID through the removal cycle; the first mutation at a
later evaluation time makes it reusable. Consumers discard removed IDs and
rebuild after missed updates.

Native rows use physical slot order. New rows reuse free slots; insertion order
is not guaranteed after removals. A physical slot can be reused during the
removal cycle while the removed row ID remains reserved. Physical slot mappings
retain storage based on peak occupancy. Compaction and capacity trimming are
not implemented yet.

A null cell is a present row with no value. ID-based cell reads return
`std::optional<T>` and throw for a missing row. Scalar batches contain cells
and validity; native time-series batches additionally contain aligned IDs.
Raw values at null positions must not be read.
Every column allows null cells. `SchemaField::optional` describes an absent
column; frame schemas require all columns and reject that flag.
Positional cell lookup scans live slots. Filtered row and batch traversal walks
current source order and tests selection membership. Selected batches combine
consecutive physical slots within cell and row-ID buffer boundaries.
Cells, validity, and batch IDs remain borrowed from source storage. Kernels should
use batches or iterate row IDs and read cells by ID to avoid repeated positional
scans. Incremental view updates use source deltas without traversing all rows.

Frame column lengths must match. A zero-column frame can carry an explicit row
count. `TSF` rows use one internal row domain across columns; schema names do
not define row identity.

Rebinding to a different row domain publishes even when both values are empty.
The row delta remains empty because no rows were added or removed.

Borrowed values and batches are valid during a node invocation, before a
mutation replaces their storage. Requesting an owning `ScalarValue` from
mutable native storage copies current cells. Ordinary mutations do not take a
snapshot. The snapshot retains values, nulls, row count, and the actual schema;
row IDs and change information remain time-series state. A Base annotation
preserves Derived columns. An explicit projection selects a narrower schema.
Assigning an immutable native snapshot to `TS<Frame>` retains its owner;
assigning a mutable TSF borrow copies its current cells into an independent
snapshot.

## TSC input/output surface

Include `<bg/frame/tsc.hpp>` for the typed views.

| View | Operations |
| --- | --- |
| `In<TSC<i64>>` | BG validity/update queries, `size`, `contains`, `cell`, `row_domain` |
| Current value | Zero-copy `value_view()`, `for_each_row`, `for_each_batch` |
| Delta | `for_each_changed` combines additions and modifications; `for_each_removed` supplies IDs |
| `Out<TSC<i64>>` | `add`, `set`, `erase`, `clear`, `invalidate` |
| `Out<TSC<i64>, frame::view>` | `bind(source)`, `invalidate` |
| `Out<TSC<i64>, frame::computed<>>` | `bind(source)`, `set(id, value)`, `set_batch(ids, batch)`, `invalidate` |

Delta reads require a modified input with matching change time. An accepted
ordinary set publishes even when the value is equal. An addition followed by
removal in one cycle cancels its row delta while retaining publication.
`clear()` supplies a valid empty value; `invalidate()` clears native rows and
removes the current value.
BG reports invalidation through `became_invalid()`, with `modified()` false
and no public row delta. Consumers discard their row state on invalidation
and rebuild from the current value on revalidation.

A `TSC` view output borrows its source cells. Its first binding reports all
visible rows as additions. Later bindings preserve source additions, removals,
and cell modifications. Rebinding to another row domain reports the old rows as
removed and the new rows as added, including overlapping numeric row IDs.
If the source changed but its delta timestamp does not match the input's last
update, the view reconciles the current rows and marks retained rows modified.

A computed `TSC` output uses `bind(source)` to follow source row membership and
row identity while owning its result cells. Source cell changes alone do not
publish the computed output; the node writes results with `set` or
`set_batch`. New rows start with null result cells. Removing a source row clears
its result cell. Accepted writes publish even when the value is equal or both
the old and new values are null. The output cell type may differ from the source
cell type. Generic `TSC<SCALAR>` declarations resolve both cell types at wiring.

## TSF input/output surface

Include `<bg/frame/frame.hpp>` for concrete and runtime-schema frames.

| View | Operations |
| --- | --- |
| `In<TSF<Schema>>` | BG validity/update queries, `size`, `contains`, `cell<Index>`, `row_domain` |
| Current value | Zero-copy `value_view()`, `for_each_row` |
| Delta | `for_each_changed`, `for_each_removed`, `for_each_modified<Index>` |
| `Out<TSF<Schema>>` | `schema`, `add`, `set`, `erase`, `clear`, `invalidate` |
| `Out<TSF<Schema>, frame::view>` | `schema`, `bind_projection(source, ordinals/names/columns<...>())`, `invalidate` |
| `Out<TSF<Schema>, frame::computed<"mid">>` | `schema`, `bind(source)`, `set<"mid">`, `set_batch<"mid">`, `invalidate` |
| Column selection | `select_column<"bid">(frame)` returns a `wp<TSC<T>>` |
| Projection helper | `project<TargetSchema, "bid", "ask">(frame)` |
| Filter helper | `filter_greater_than<"size">(frame, threshold)` |

The selected column shares native cell storage with its frame. A row membership
change publishes every child column; a cell patch publishes the changed child
columns and the frame. A child consumer is scheduled only when its column is
published. `TSF::value_view()` exposes the current scalar frame during the node
invocation; an owning `ScalarValue<Frame<Schema>>` snapshots its cells.

A `view` output borrows source column storage and keeps the requested rows in
current source order. `bind_projection` accepts a span of source ordinals, a
span of `util::symbol` names, or `columns<...>()`. Each maps source columns to
target fields in target schema order. Source and target cell types must match.
The first binding reports all
current rows as added. Later bindings forward additions and removals and report
changes only for mapped columns. Rebinding within the same row domain reconciles
membership and reports changed column bindings as modifications to retained rows.
Rebinding to a different row domain reports old rows as removed and new rows as
added, including overlapping numeric IDs.

A valid TSF view output uses either projection or filtering. Switching between
these operations throws `std::logic_error`; invalidate the output before
changing its operation.

### Mixed computed and borrowed columns

`Out<TSF<Schema>, frame::computed<Names...>>` owns the named computed columns
and borrows the remaining columns. `bind(source)` matches borrowed fields by
name and exact cell type. The source can omit computed fields and use a different
schema order. Both concrete and runtime schemas use the same output implementation.
Computed names must be distinct and present in the resolved output schema;
wiring rejects invalid names.

The output preserves source row IDs, row domain, and current traversal order.
Computed columns share one local physical slot map. New computed cells start
null. Removing a row clears its computed cells before releasing its slot.
Rebinding to a different domain clears computed cells and reports the old rows
removed and the new rows added. See the
[storage decision](docs/decisions/computed-column-storage.md).

Use `set<Name>(id, value)` with a numeric value, `std::optional<T>`, or
`std::nullopt`. `set_batch<Name>(ids, batch)` writes a `ColumnBatch<T>` after
validating its cell type, row IDs, length, and value-buffer presence. An all-null
batch can use a null value pointer. Batch publication occurs once. Accepted
writes publish even when the cell value is equal, following ordinary time-series
semantics. Cell writes publish their computed column and the frame. Membership
changes publish every column. A later cell write can publish without another
`bind` when membership is unchanged.

```cpp
output.bind(source);
output.set<"mid">(row, 101.5);
output.set_batch<"mid">(rows, batch);
```

### Projection and filtering helpers

`project<TargetSchema, "bid", "ask">(frame)` creates a projection node that
binds the named source columns to the target schema in order. The projection
shares the source cells.

`filter_greater_than<"size">(frame, threshold)` creates a filter node with a
numeric scalar threshold. The filter retains the source schema and shares its
column storage. It tests added rows and rows whose predicate column changed.
Changes to other columns are forwarded for selected rows without reevaluating
the predicate. A selected column view covers only the filter's visible rows.
If the source changed but its delta timestamp does not match the input's last
update, the filter reconciles current membership and reevaluates its predicate.
The predicate must remain stable while the binding is retained. Changing a
captured threshold does not signal a rebuild; dynamic thresholds and expression
kernels are later work.
The existing `bind_filter` callback and `filter_greater_than` helper are numeric
probes. The planned filter API belongs to nodes, evaluates expression blocks,
and uses BG materialisation options.

```cpp
template <template <typename, auto...> typename Field> struct BidOnly {
    Field<double> price;
};

struct BidProjection {
    bg::In<bg::frame::TSF<Quotes>> source;
    bg::Out<bg::frame::TSF<BidOnly>, bg::frame::view> output;

    void evaluate() const { output.bind_projection(source, bg::frame::columns<"bid">()); }
};
```

`In<TSC<T>>::extension_descriptor()` and `In<TSF<Schema>>::extension_descriptor()`
are extension implementation APIs. Their descriptor types are in
`bg::frame::details`; node code uses typed `In` and `Out` operations.

```cpp
template <template <typename, auto...> typename Field> struct Quotes {
    Field<bg::i64> size;
    Field<double> bid;
    Field<double> ask;
};

struct QuoteWriterState {
    bg::frame::row_id_t row{bg::frame::no_row};
};

struct QuoteWriter {
    bg::In<bg::TS<bg::i64>> command;
    bg::State<QuoteWriterState> state;
    bg::Out<bg::frame::TSF<Quotes>> output;

    void evaluate() const {
        if (*command == 1) {
            state->row = output.add(bg::frame::Row<Quotes>{
                .size = bg::frame::Cell<bg::i64>{100}, .bid = bg::frame::Cell<double>{101.25},
                .ask = std::nullopt});
        } else if (*command == 2) {
            output.set(state->row, bg::frame::RowPatch<Quotes>{.bid = 101.5});
        } else if (*command == 3) {
            output.set(state->row, bg::frame::RowPatch<Quotes>{});
        }
    }
};

const auto write = bg::node<QuoteWriter>();
const auto quote_stream = write(command);
consume_bid(bg::frame::select_column<"bid">(quote_stream));
```

Column names come from the aggregate members of `Schema<Column>`. The numeric
`select_column<Index>` overload is available when schema order is intended.
`add(Row<Schema>)` supplies every field as a value or null.
`add(RowPatch<Schema>)` inserts a row with nulls for omitted fields; an empty
patch inserts an all-null row. `set(id, RowPatch<Schema>)` leaves omitted
fields unchanged. Setting a field to `std::nullopt` clears that cell.
An empty modification patch does not publish an update.

`NativeColumn`, `NativeFrame`, scalar storage holders, and provider descriptors are
extension-facing implementation APIs. Node code uses `In`, `Out`, and scalar
views.

## Runtime schema values and generic input/output APIs

`make_frame_value(schema, row_count, columns)` returns
`ScalarValue<Frame<SCHEMA>>`. The schema uses BG's registered `bg::Schema`
descriptor. Columns are supplied in schema order as borrowed `ColumnView<>`
arguments. The factory copies their cells into an independent owning snapshot
and validates column count, lengths, and cell types. A zero-column frame retains
the explicit row count. An overload accepts a caller-owned allocator.

```cpp
const auto fields = std::array{
    bg::SchemaField{bg::util::symbol{"size"}, &bg::cpp_scalar_type<bg::i64>()},
    bg::SchemaField{bg::util::symbol{"bid"}, &bg::cpp_scalar_type<double>()}};
const auto &schema = bg::anonymous_schema(fields);
const auto sizes = bg::frame::Column<bg::i64>{{10, 20}};
const auto bids = bg::frame::Column<double>{{12.5, std::nullopt}};
const auto columns = std::array{
    bg::frame::ColumnView<>{sizes.view()}, bg::frame::ColumnView<>{bids.view()}};
const auto value = bg::frame::make_frame_value(schema, 2, columns);
const auto bid = value.view().value().column("bid").view<double>();
```

`In<TSF<SCHEMA>>::schema()` returns the actual resolved schema. Its
`column(name)` and `column(ordinal)` operations return `In<TSC<SCALAR>>`.
`input<T>()` selects a checked typed time-series input; `view<T>()` selects a
checked scalar column view. `value_view()` preserves the declared Frame or
Column family and borrows the actual source representation.
Time-series cells use row IDs; scalar-view cells use positions.

A C++ schema field declared as `Field<SCALAR>` exposes `ColumnView<>` through
`value.columns().field`. Use its checked `view<T>()` operation. The numeric
`In<TSF>::cell<Index>` convenience requires a known numeric field; generic
fields use `column(name).input<T>().cell(id)`.

`select_column<"bid">(wp<TSF<SCHEMA>>)` resolves the field ordinal from the actual
wiring-point schema and returns `wp<TSC<SCALAR>>` with the resolved child type.

Generic columns use the same `i64` and `double` storage and batch operations as
concrete declarations. Runtime schemas resolve during wiring and remain fixed
for the graph lifetime.

`Out<TSF<SCHEMA>>::schema()` exposes the resolved output schema. Generic writers
accept borrowed scalar arguments:

```cpp
output.add(std::span<const std::optional<bg::Scalar<>>>{row});
output.set(id, std::span<const std::pair<bg::util::symbol,
                                      std::optional<bg::Scalar<>>>>{patch});
```

Complete scalar rows follow output schema order; empty optionals mean null.
Named patches leave omitted fields unchanged and reject duplicate names.
`Row<Schema>` and `RowPatch<Schema>` arguments also work on generic outputs.
Their names and cell types must match the complete output schema; the adapter
maps fields into output schema order. All argument validation runs before
mutation.

A field declared with `SCALAR` or another `ScalarVariable` accepts a borrowed
`Scalar<>` in Row/RowPatch arguments. Concrete numeric fields retain typed
nullable values. The source schema's variable constraints and repeated-variable
bindings must match the resolved output fields, including omitted patch fields.
Each present handle must have the target cell type. Mutation copies its numeric
value into table storage; the table retains no argument handles.

```cpp
template <template <typename, auto...> typename Field> struct GenericQuote {
    Field<bg::i64> size;
    Field<bg::SCALAR> bid;
};

const auto price = 12.5;
const auto price_view = bg::Scalar<double>::from(price);
const auto id = output.add(bg::frame::Row<GenericQuote>{
    .size = bg::frame::Cell<bg::i64>{10}, .bid = bg::Scalar<>::from(price_view)});
output.set(id, bg::frame::RowPatch<GenericQuote>{.bid = std::nullopt});
```

Borrowed argument values must remain valid until the mutation call returns.
[HANDOFF.md](HANDOFF.md) records verification and remaining work.

## Arrow values

Include `<bg/frame/arrow.hpp>` and link `bg::frame_arrow`.

| Operation | Result and ownership |
| --- | --- |
| `borrow_arrow(column_view)` | Arrow `ChunkedArray` sharing compatible value spans. |
| `borrow_arrow(frame_view)` | Arrow `Table` with independently chunked columns. |
| `copy_to_arrow(column_view)` | Arrow `ChunkedArray` with one dense owning chunk. |
| `copy_to_arrow(frame_view)` | Arrow `Table` with one dense owning chunk per column. |
| `from_arrow<T>(chunked_array)` | Owning erased `ScalarValue<>` with semantic type `Column<T>`. |
| `from_arrow<Schema>(table)` | Owning erased `ScalarValue<>` with semantic type `Frame<Schema>`. |
| `from_arrow(table)` | Owning erased `ScalarValue<>` with an inferred anonymous Frame schema. |
| `from_arrow(table, schema)` | Owning erased `ScalarValue<>` with the supplied resolved BG Frame schema. |

These functions return `arrow::Result`. Exports preserve current positional
order and nulls. They contain scalar values; internal row IDs and time-series
deltas are not part of the exported schema.

A borrowed Arrow view has the same lifetime as the scalar view: use it during
the node invocation, before source mutation. It may allocate chunk metadata
and packed validity bitmaps. An all-null native span without allocated cells
needs a synthetic value buffer. Borrowing never switches to gathering because
of fragmentation.

`copy_to_arrow` gathers the current rows into fresh value and validity buffers,
including when the source is already Arrow-backed. It initializes value bytes
at null positions. The result remains valid after source mutation or
destruction and can be passed outside the graph.

Imports accept CPU `int64` and `float64` arrays, including sliced and chunked
arrays. Explicit Frame schemas must match the table's names, order, and types.
Inferred schemas preserve field order and reject duplicate names. Arrow field
nullability describes cells; every BG schema column is present. Concrete-schema
imports use the same runtime storage as programmatic schemas. Frame exports
preserve the actual schema, including Derived fields observed through Base.
Generic `ColumnView<>` values support both export operations.
Each imported scalar retains its Arrow owners; the caller must supply buffers
whose lifetime those owners manage. Importing a borrowed export does not
extend the BG source lifetime. Use `copy_to_arrow` to retain that data.

Arrow-backed scalar reads use the same `ColumnView` and `FrameView` interfaces
as native storage. An erased owning scalar copy preserves the Arrow runtime.
A typed owning scalar copy follows BG's canonical storage traits and copies
the cells into native snapshot storage. Import does not create a mutable TSF
or assign internal row IDs.

## Initialization

The native representation follows BG's position-independent initial storage
contract. Its empty header contains bunches, indexes, counters, and an unbound
scalar descriptor. Mutations and cleanup receive the output allocator.
The scalar descriptor binds to the actual runtime column before a valid
value is exposed. Byte-copy initialization applies only to the empty header.

Immutable snapshots are allocated when an owning value is requested. Their
storage retains its allocator for cleanup and is separate from the graph's
initial output image.

Extension implementations must pass the same allocator to native mutations
and `release()`. The time-series runtime performs that cleanup; native headers
have trivial destruction for BG initialization.

## Provider integration

`bg_frame_provider_integration_test` defines a compact external time-series
provider. It owns its declaration, semantic descriptor, runtime descriptor,
and typed `In` and `Out` views. The test registers its default runtime,
resolves that runtime through the installed package, verifies the exact
`In<TS_T>` and `Out<TS_T>` erased fallbacks, and evaluates callable and
aggregate nodes.

The provider defines distinct default and `provider::view` runtimes. A default
output always selects its default writable runtime after a view input. A view
output selects its provider runtime and invalidates through its configured
output declaration.

The fixture's `provider::view` runtime copies an `i64` into distinct output storage.
The column tests cover borrowed batches, stable scalar snapshots, recycled
IDs, null cells, sparse blocks, and allocation-failure cleanup. TSC graph tests
cover empty publication, row deltas, invalidation, and revalidation. Two runtime
graphs created from one compiled graph verify independent storage and allocator
cleanup.

Provider descriptors describe runtime storage and binding. Extension code uses
`TimeSeriesType`, `TimeSeriesRuntimeType`, `TimeSeriesInputData`,
`TimeSeriesState`, `TimeSeriesOutputBinding`, and `ScalarData` for that work.
Inputs expose `valid()`, `became_valid()`, `became_invalid()`, `modified()`,
and `last_updated()` from the bound state. Node definitions use typed `In` and
`Out` views and `bg::node`; they do not access output state or runtime storage
directly.

## Build against an installed BG package

Configure the extension with a BG installation prefix. The package config also
finds its `ddvisitor` dependency. Set `BG_INSTALL_PREFIX` to the directory
containing the installed `lib64/cmake/bg/bgConfig.cmake`.
The current lane uses installed BG `a8af8eef` and Arrow 24.0.0.
[HANDOFF.md](HANDOFF.md) lists the exact package prefix, verified checkpoints,
and remaining foundation work.

```sh
BG_INSTALL_PREFIX=/path/to/bg-install
cmake -S extensions/frame -B extensions/frame/build-installed \
  -DCMAKE_PREFIX_PATH="$BG_INSTALL_PREFIX" \
  -DBUILD_TESTING=ON
cmake --build extensions/frame/build-installed -j12
ctest --test-dir extensions/frame/build-installed --output-on-failure
```

Enable Arrow support with `-DBG_FRAME_BUILD_ARROW=ON` and add the Arrow
installation prefix to `CMAKE_PREFIX_PATH`:

```sh
cmake -S extensions/frame -B extensions/frame/build-arrow \
  -DCMAKE_PREFIX_PATH="$BG_INSTALL_PREFIX;$ARROW_INSTALL_PREFIX" \
  -DBG_FRAME_BUILD_ARROW=ON -DBUILD_TESTING=ON
cmake --build extensions/frame/build-arrow -j12
ctest --test-dir extensions/frame/build-arrow --output-on-failure
```

The adapter requires the `Arrow::arrow_shared` CMake target. Arrow support is
disabled by default; native frame builds do not require Arrow.

The sanitizer build used for lifetime checks is:

```sh
cmake -S extensions/frame -B extensions/frame/build-sanitizers \
  -DCMAKE_PREFIX_PATH="$BG_INSTALL_PREFIX;$ARROW_INSTALL_PREFIX" \
  -DBG_FRAME_BUILD_ARROW=ON -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build extensions/frame/build-sanitizers -j12
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir extensions/frame/build-sanitizers --output-on-failure
```

The current foundation passed five Arrow-enabled suites, four Arrow-disabled
suites, and all five Arrow-enabled suites under AddressSanitizer and
UndefinedBehaviorSanitizer. LeakSanitizer is disabled because of this
environment's ptrace restriction; allocation-failure tests still verify
allocator cleanup.

## Performance sample

The [results](benchmarks/arrow_benchmark_results.md) and
[CSV](benchmarks/arrow_benchmark_results.csv) measure a real graph:
source mutation, incremental filtering, column selection, and a full column
sum. They compare native batches, borrowed Arrow export plus sum, and dense
gather-copy plus sum. Export timing includes allocation and destruction.

Build the optional benchmark with Arrow and ArrowCompute installed:

```sh
cmake -S extensions/frame -B extensions/frame/build-benchmark-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$BG_INSTALL_PREFIX;$ARROW_INSTALL_PREFIX" \
  -DBG_FRAME_BUILD_ARROW=ON -DBG_FRAME_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF
cmake --build extensions/frame/build-benchmark-release -j12
extensions/frame/build-benchmark-release/bg_frame_arrow_benchmark \
  --report extensions/frame/benchmarks/arrow_benchmark_results.md \
  --csv extensions/frame/benchmarks/arrow_benchmark_results.csv
```

This sample uses an optimized extension and benchmark with a Debug BG library.
It supports comparisons between the consumer paths. Measure with a Release BG
library before drawing conclusions about optimized whole-graph throughput.
