# Column expressions and table transformations

Column expressions apply one calculation to all current rows and maintain the result
when rows change. They return `TSC` connections that can feed ordinary graph nodes,
reductions, and further expressions.

Include `<bg/frame/frame.hpp>`, `<bg/frame/expression.hpp>`, and
`<bg/library.hpp>`. Import
`bg::frame` for expressions and `bg::literals` for named arguments. Build and link
the [frame extension](index.md#frame).

## Selecting and calculating columns

`select_column<"name">(frame)` selects one typed column. Matching numeric columns
support arithmetic and comparisons. A constant or a `TS<T>` can supply one value for
every row:

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

`col("name")` describes a column before a frame is available. `bind_expression` resolves
names against a frame and creates the calculation. This separates reusable formulas
from their graph inputs:

```cpp
auto spread_formula = col("ask") - col("bid");
auto spread = bind_expression(quotes, spread_formula);
```

Constants are copied when the expression is constructed.

Named parameters allow one expression to bind to different graph inputs:

```cpp
using namespace bg::literals;

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

`frame::filter(frame, predicate)` retains rows whose predicate is true. False and null
exclude a row. Membership updates when cells or captured parameters change.

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
#include <bg/frame/expression.hpp>
#include <bg/frame/frame.hpp>
#include <bg/frame/literal.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install -DBG_SOURCE_DIR=/path/to/bg
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

## Adding or replacing columns

`with_columns` preserves the source rows and calculates a result schema from named
assignments. Assignments can be column expressions, column connections, scalar values,
or scalar time series:

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
