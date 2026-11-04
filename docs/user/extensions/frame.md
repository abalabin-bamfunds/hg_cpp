# Columns and frames

If your calculations work across rows of nullable data, use the frame extension for
columns and tables. Scalar columns and frames suit complete snapshots, while their
incremental forms let rows arrive, change, or disappear independently. The extension
currently provides a C++ API; see the [build requirements](index.md#frame).

| Type | Current value | Update |
| --- | --- | --- |
| `TS<Column<T>>` | One nullable column | Complete replacement |
| `TS<Frame<Schema>>` | Named columns with equal lengths | Complete replacement |
| `TSC<T>` | Nullable cells with row identities | Row additions, modifications, and removals |
| `TSF<Schema>` | Named `TSC` columns sharing rows | Row additions, field patches, and removals |

Cell types are `bool`, `i64`, `double`, `string`, and `symbol`. Every column permits
null cells. Null means a present cell without a value; it differs from a removed row
and from an invalid time series. An empty string is a valid value.

## Scalar values

For a complete column or table that updates as one value, use `Column<T>` or
`Frame<Schema>`. These immutable owning scalars keep the data together, with the
frame schema declaring each column's name and cell type. Include
`<hg/frame/column.hpp>`, `<hg/frame/frame.hpp>`, and `<hg/frame/literal.hpp>`.

```cpp
using namespace hg;
using namespace hg::frame;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<double> bid;
    Field<double> ask;
};

auto prices = ScalarValue<Column<double>>::from_literal({1.0, NULL_VALUE, 3.0});
auto quotes = ScalarValue<Frame<Quotes>>::from_literal({
    {"bid", {1.0, NULL_VALUE}},
    {"ask", {1.5, 2.5}},
});
```

A frame literal supplies every column by name. Columns must have equal lengths and
cell types must match the schema. Ordinary `TS<Column<T>>` and `TS<Frame<Schema>>`
accept these literals in `run_graph` inputs. Scalar equality compares schema, row
count, nulls, and ordered values.

`ColumnView<T>` and `FrameView<Schema>` borrow scalar data during a node invocation.
An owning `ScalarValue` copied from a mutable frame view retains a snapshot for later use.

## Keyed row updates

If application keys identify records, use a keyed `TSF` so updates can insert or patch
those records. Schema metadata declares the key columns; omitted non-key fields are
null on insertion and unchanged on a patch, while `REMOVE` deletes a row.

<!-- example-libraries: hg::frame -->
<!-- example-source: extensions/frame -->
<!-- runnable-example: cpp -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/frame/frame.hpp>
#include <hg/frame/literal.hpp>
#include <hg/library/operators.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::frame;
using namespace hg::operators;
using namespace literal;

template <template <typename, auto...> typename Field> struct Trades {
    Field<i64> id;
    Field<double> price;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

int main() {
    const auto total = [](wp<TSF<Trades>> trades) {
        return sum_(select_column<"price">(trades));
    };
    std::cout << run_graph(total, {
        {{{"id", i64{1}}, {"price", 10.0}}, {{"id", i64{2}}, {"price", 20.0}}},
        {{{"id", i64{1}}, {"price", 12.0}}},
        {REMOVE({{"id", i64{2}}})},
    }) << '\n';
}
```

Output:

```text
[30, 32, 12]
```
:::

:::{tab-item} Build and run

The [C++ example project](index.md#frame) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages" -DHG_SOURCE_DIR=/path/to/hg
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::


Keys can contain `i64`, `string`, or `symbol` cells, including nulls. Duplicate live
keys are rejected. A patch supplies every key field and cannot change the key.
Remove the old row and insert the new key to change it.

## Appending rows

For incoming rows that do not have application keys, use an unkeyed `TSF` to append
each literal row. If only one column is needed, a `TSC<T>` literal appends nullable
cells in the same way:

```cpp
auto sizes = [](wp<TSC<i64>> values) { return operators::len_(values); };
auto result = run_graph(sizes, {{i64{10}, NULL_VALUE}, {i64{20}}});
```

This graph publishes row counts of two and three. Null cells count as rows. Existing
unkeyed rows can be modified or removed through the [node API](frame-nodes.md).
Internal row IDs identify rows within one input's row domain; they are separate from
application keys. Consumers must discard removed IDs.

## Empty, skipped, and invalid inputs

To distinguish an empty table from an update gap or a withdrawn value, use the
incremental input markers. An empty literal makes an invalid input valid and empty,
but leaves an already valid input unchanged; `SKIP` leaves the input as it is, and
`INVALID` removes its current value. The markers come from `literal`.

```cpp
using namespace literal;

auto result = run_graph(sizes, {{i64{10}}, SKIP, INVALID, {}});
```

The row counts are `[1, None, None, 0]`. Invalidation clears the input; the final
empty literal starts a new valid empty column.

## Queries and reductions

To summarize current rows or inspect their membership, use the frame overloads of
the existing library operators:

| Operator | Result |
| --- | --- |
| `len_(frame_or_column)` | Number of current rows, including null cells |
| `is_empty(frame_or_column)` | Whether there are no current rows |
| `sum_(numeric_column)` | Sum of non-null cells in `TSC<i64>` or `TSC<double>` |
| `valid(value)` | Whether the root time series is valid |
| `all_valid(value)` | Whether the root and, for a frame, each immediate column are valid |
| `count(value)` | Number of root ticks |

The sum is zero for a valid empty or all-null column. Invalidation invalidates queries
and sums. Column updates can publish an unchanged row count. Floating-point sums use
incremental arithmetic and can differ from a fresh scan through rounding.

## Testing and printing

To check how row updates affect a graph, pass row literals to `run_graph` and compare
its captured deltas. Keyed frame expectations match rows by key and patches by
modified field; unkeyed expectations name rows so later updates can refer to them
without predicting internal IDs:

```cpp
auto matches = actual.equals({
    {{"a", 10.0}, {"b", 20.0}},
    {{"a", 11.0}},
    {{"b", REMOVED}},
});
```

Each expected tick describes the complete delta. Use `SKIP` for no publication and
`INVALID` for invalidation. Printed incremental results include internal row IDs;
printed scalar columns show nullable cell values.

For row calculations and table transformations, see
[column expressions](frame-expressions.md). To maintain totals for each venue or
other key, use [grouped aggregation](frame-grouping.md). For custom row processing
and aggregate implementations, see [frame nodes and interchange](frame-nodes.md).
