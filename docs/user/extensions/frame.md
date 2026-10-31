# Columns and frames

The frame extension maintains calculations over nullable columns and tables.
Use scalar columns and frames when each update replaces a complete value. Use
incremental columns and frames when rows arrive, change, or disappear independently.
The extension currently provides a C++ API. See [build requirements](index.md#frame).

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

`Column<T>` and `Frame<Schema>` are immutable owning scalars. A frame schema declares
the name and cell type of each column. Include `<bg/frame/column.hpp>`,
`<bg/frame/frame.hpp>`, and `<bg/frame/literal.hpp>`.

```cpp
using namespace bg;
using namespace bg::frame;

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

Use a keyed `TSF` when application keys identify records. Schema metadata declares the
key columns. An update inserts an absent key or patches a present key. Omitted
non-key fields are null on insertion and unchanged on a patch. `REMOVE` deletes a row.

<!-- example-libraries: bg::frame -->
<!-- example-source: extensions/frame -->
<!-- runnable-example: cpp -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/frame/frame.hpp>
#include <bg/frame/literal.hpp>
#include <bg/library/operators.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::frame;
using namespace bg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install -DBG_SOURCE_DIR=/path/to/bg
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

An unkeyed `TSF` appends each literal row. A `TSC<T>` literal appends nullable cells:

```cpp
auto sizes = [](wp<TSC<i64>> values) { return operators::len_(values); };
auto result = run_graph(sizes, {{i64{10}, NULL_VALUE}, {i64{20}}});
```

This graph publishes row counts of two and three. Null cells count as rows. Existing
unkeyed rows can be modified or removed through the [node API](frame-nodes.md).
Internal row IDs identify rows within one input's row domain; they are separate from
application keys. Consumers must discard removed IDs.

## Empty, skipped, and invalid inputs

For incremental inputs, an empty literal validates an invalid input as an empty value.
It makes no change to an already valid value. `SKIP` leaves the input unchanged;
`INVALID` removes its current value. These markers come from `literal`.

```cpp
using namespace literal;

auto result = run_graph(sizes, {{i64{10}}, SKIP, INVALID, {}});
```

The row counts are `[1, None, None, 0]`. Invalidation clears the input; the final
empty literal starts a new valid empty column.

## Queries and reductions

The extension supplies overloads of existing library operators:

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

`run_graph` captures row deltas. Keyed frame expectations match rows by key and patches
by modified field. Unkeyed expectations assign names to rows so later expectations can
refer to the same row without predicting internal IDs:

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
[column expressions](frame-expressions.md). For custom row processing, see
[frame nodes and interchange](frame-nodes.md).
