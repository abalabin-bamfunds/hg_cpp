# Grouped frame aggregation

If you need totals by venue, instrument, or another set of fields, use
`group_by(frame, names...).agg(assignments...)`. It keeps one row per nonempty
group and updates the affected totals as source rows change. Ordinary row updates
process changed rows, so the graph can maintain summaries without repeatedly
summing the entire table.

The frame extension currently provides this API in C++. Include
`<hg/frame/frame.hpp>` and `<hg/library.hpp>`, and use the
[frame build setup](index.md#frame).

## Totals by group

To maintain several totals over the same groups, pass named `sum_` expressions to
`agg`. Each expression can read a column or calculate a value from several columns.
In this example, quantity and notional share the venue grouping:

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
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::frame;
using namespace hg::literals;
using namespace literal;

template <template <typename, auto...> typename Field>
struct Trades {
    Field<i64> id;
    Field<symbol> venue;
    Field<i64> size;
    Field<double> price;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

int main() {
    const auto totals = [](wp<TSF<Trades>> trades) {
        return frame::group_by(trades, "venue").agg(
            "quantity"_n = operators::sum_(col("size")),
            "notional"_n = operators::sum_(
                col("price") * frame::convert<double>(col("size"))));
    };
    std::cout << run_graph(totals, {
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{5}}, {"price", 2.0}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{7}}, {"price", 3.0}}},
        {{{"id", i64{1}}, {"size", i64{8}}}},
        {{{"id", i64{2}}, {"venue", symbol{"Y"}}}},
        {REMOVE({{"id", i64{1}}})},
    }) << '\n';
}
```

Output:

```text
[{0: {"venue": "X", "quantity": 12, "notional": 31}}, {0: {"venue": "X", "quantity": 15, "notional": 37}}, {1: {"venue": "Y", "quantity": 7, "notional": 21}, 0: {"venue": "X", "quantity": 8, "notional": 16}}, {0: REMOVE}]
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

The first update creates venue X with quantity 12 and notional 31. Changing trade 1
updates both totals. Moving trade 2 to venue Y subtracts its contribution from X
and creates Y. Removing the final trade in X removes that group.

Printed integers before each row identify output rows; they are separate from the
source trade IDs. A group's row ID stays stable while it exists. Output order is
unspecified, so code should identify a group by its key fields rather than position.

## Grouping keys and result fields

To group by a combination of fields, pass their names in key order, for example
`group_by(trades, "venue", "instrument")`. Names may also come from a wiring-time
collection. To group by a calculated value, add it with
[`with_columns`](frame-expressions.md#adding-or-replacing-columns) first.

Grouping supports `i64`, `string`, and `symbol` columns. Null grouping cells compare
equal. The result schema contains the grouping fields, followed by the aggregate
fields in assignment order. Its ordered key tuple consists of the grouping fields.
Both declared C++ schemas and schemas defined during wiring are supported.

## Changing calculation parameters

If all rows use a changing factor, capture its scalar time series in an aggregate
expression. A factor update recalculates that aggregate for all current rows, while
ordinary cell updates affect only the relevant row contributions:

::::::{tab-set}
:sync-group: language

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/frame/frame.hpp>
#include <hg/frame/literal.hpp>
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::frame;
using namespace hg::literals;
using namespace literal;

template <template <typename, auto...> typename Field>
struct Trades {
    Field<i64> id;
    Field<symbol> venue;
    Field<i64> size;
    Field<double> price;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

int main() {
    const auto adjusted = [](wp<TSF<Trades>> trades, wp<TS<double>> multiplier) {
        return frame::group_by(trades, "venue").agg(
            "notional"_n = operators::sum_(
                col("price") * frame::convert<double>(col("size")) * multiplier));
    };
    std::cout << run_graph(adjusted, {
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{5}}, {"price", 2.0}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{7}}, {"price", 3.0}}},
        SKIP,
        {{{"id", i64{2}}, {"size", i64{10}}}},
    }, {1.0, 2.0, SKIP}) << '\n';
}
```

Output:

```text
[{0: {"venue": "X", "notional": 31}}, {0: {"venue": "X", "notional": 62}}, {0: {"venue": "X", "notional": 80}}]
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

A captured parameter must be valid before the result can be calculated. Its
invalidation invalidates the grouped result; revalidation recalculates from the
current source rows. Source invalidation also invalidates the result and discards
retained group state; revalidation rebuilds it. A reference switch reconciles the
sampled rows and preserves groups that remain nonempty.

## Numeric sum rules

To sum nullable numeric data, use the same `sum_` operation for `i64` or `double`
expressions. Null cells do not contribute, and an all-null group sums to zero.
Integer sums wrap at the scalar width.

Double sums use compensated accumulation for finite values, but remain dependent
on update order. A NaN contribution, or both positive and negative infinity,
produces NaN. Removing those contributions restores the result from the remaining
contributions. These rules also apply to standalone column sums.

`operators::agg(grouped, assignments...)` is equivalent to `grouped.agg(assignments...)`.

For operations beyond sums, extension authors can register
[custom grouped aggregates](frame-nodes.md#custom-grouped-aggregates).
