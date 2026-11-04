# Column expressions and table transformations

If you need the same calculation for every row of a table, use column expressions
to describe it once and keep the result current as rows change. Their `TSC` outputs
can feed ordinary graph nodes, reductions, and further expressions.

Include `<hg/frame/frame.hpp>`, `<hg/frame/expression.hpp>`, and
`<hg/library.hpp>`. Import
`hg::frame` for expressions and `hg::literals` for named arguments. Build and link
the [frame extension](index.md#frame).

## Selecting and calculating columns

To calculate from a frame field, select its typed column with
`select_column<"name">(frame)` and use arithmetic or comparisons on matching numeric
columns. A constant or `TS<T>` can supply a shared value for every row:

```cpp
auto bid = select_column<"bid">(quotes);
auto ask = select_column<"ask">(quotes);
auto midpoint = (bid + ask) / 2.0;
auto total = operators::sum_(midpoint);
```

Integer and double operands must match; mixed arithmetic requires an explicit
conversion. Division returns double. A null operand produces a null arithmetic or
comparison result. String and symbol columns support equality and inequality with
matching types.

```cpp
auto as_double = operators::convert[type<TSC<double>>](size_column);
```

## Expressions by name

If the same formula should work with different frame inputs, describe its columns
by name with `col("name")`. `bind_expression` then resolves those names against a
frame and connects the calculation:

```cpp
auto spread_formula = col("ask") - col("bid");
auto spread = bind_expression(quotes, spread_formula);
```

Constants are copied when the expression is constructed.

Named parameters allow one expression to bind to different graph inputs:

```cpp
using namespace hg::literals;

auto formula = col("ask") - col("bid") < param("limit");
auto narrow = bind_expression(quotes, formula, "limit"_n = tolerance);
auto numeric_size = bind_expression(quotes, frame::convert<double>(col("size")));
```

`tolerance` is a `TS<double>` connection. Its updates recalculate every current row;
column updates recalculate affected rows. An invalid time series parameter invalidates
the result.

Explicit named bindings take precedence over frame columns. Missing, duplicate, and
unused bindings fail during wiring. A calculation needs at least one column to define
its rows.

## Filtering rows

To keep only rows that meet a changing condition, use `frame::filter(frame, predicate)`.
Rows enter or leave the result as cells or captured parameters change; false and
null predicates exclude a row.

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
#include <hg/frame/expression.hpp>
#include <hg/frame/frame.hpp>
#include <hg/frame/literal.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::frame;
using namespace hg::literals;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<i64> id;
    Field<double> bid;
    Field<double> ask;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

int main() {
    const auto mids = [](wp<TSF<Quotes>> quotes) {
        const auto priced = with_columns(quotes, "mid"_n = (col("bid") + col("ask")) / 2.0);
        const auto selected = filter(priced, col("mid") > 100.0);
        return select_column<"mid">(selected);
    };
    std::cout << run_graph(mids, {
        {{{"id", i64{1}}, {"bid", 99.0}, {"ask", 103.0}},
         {{"id", i64{2}}, {"bid", 100.0}, {"ask", 104.0}}},
        {{{"id", i64{1}}, {"bid", 101.0}}},
    }) << '\n';
}
```

Output:

```text
[{0: 101, 1: 102}, {0: 102}]
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


Boolean expressions support `&&`, `||`, and `!`. False determines an AND result; true
determines an OR result; otherwise a null operand produces null. These operations keep
all graph dependencies active. They do not short-circuit dependency evaluation.

Column operands must share the same row domain and selection. Columns selected from one
frame, its projections, and its computed columns meet this requirement. Independently
filtered frames have different selections, even when their current rows happen to match.

## Grouped sums

If a table needs one total per venue or another key, use
[`group_by(...).agg(...)`](frame-grouping.md). It accepts the same column expressions
and keeps grouped totals current as rows are added, changed, moved, or removed.

## Adding or replacing columns

To attach calculated fields while keeping the source rows, use `with_columns` with
named assignments. Each assignment can be a column expression, column connection,
scalar value, or scalar time series:

```cpp
auto priced = frame::with_columns(
    quotes,
    "mid"_n = (col("bid") + col("ask")) / 2.0,
    "venue"_n = symbol{"LSE"},
    "limit"_n = tolerance);
```

An assignment replaces an existing field or appends a new field. Existing fields retain
their order; new fields follow assignment order. Expressions read the source frame.
Chain calls when a calculation needs a field added by an earlier call. Key fields
cannot be replaced.

## Selecting, renaming, and dropping fields

To expose only the fields a consumer needs, use `select`, `rename`, or `drop` to
shape the frame's schema while preserving its row identities:

```cpp
auto prices = frame::select(priced, "bid", "ask", "mid");
auto renamed = frame::rename(prices, {{"mid", "midpoint"}});
auto displayed = frame::drop(renamed, "ask");
```

Name-only selections, renames, and drops borrow source columns. A `select` assignment
can calculate a field expression. These operations preserve row identities and publish
changes for the fields they expose. Their schemas resolve during wiring. Use
`project<TargetSchema, "bid", "ask">(quotes)` when the result needs a declared C++
schema. A keyed projection must retain its complete key in key order; an unkeyed
projection can omit key fields.
