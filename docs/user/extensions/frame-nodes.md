# Frame nodes and interchange

Custom nodes can read current rows, process changed rows, or publish row updates.
Use the [column operations](frame-expressions.md) for calculations that those operations
already express.

## Reading current values and changes

`In<TSC<T>>` and `In<TSF<Schema>>` provide the ordinary BG validity and modification
queries plus row access. Null cell reads return `std::optional`; reading a missing row
throws. Check optional cells before reading their values.

| Read | Purpose |
| --- | --- |
| `size()`, `contains(id)` | Inspect current row membership |
| `value_view()` | Borrow a scalar view during this invocation |
| `for_each_row(callback)` | Visit current rows |
| `for_each_changed(callback)` | Visit added or modified rows in this cycle |
| `for_each_removed(callback)` | Visit removed row IDs |
| `cell(id)` on a column | Read one nullable cell |
| `cell<Index>(id)` on a frame | Read one nullable field |

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
#include <bg/frame/tsc.hpp>
#include <bg/frame/literal.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::frame;
using namespace literal;

int main() {
    const auto sum_values = node([](In<TSC<double>> values, Out<TS<double>> total) {
        auto sum = 0.0;
        values.for_each_row([&](row_id_t id, std::size_t) {
            if (const auto value = values.cell(id)) {
                sum += *value;
            }
        });
        total.set(sum);
    });
    const auto graph = [&](wp<TSC<double>> values) { return sum_values(values); };

    std::cout << run_graph(graph, {{10.0, NULL_VALUE}, {12.0}}) << '\n';
}
```

Output:

```text
[10, 22]
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


Row IDs belong to a row domain and may be reused after removal. A node retaining values
by ID must discard removed entries. Removed IDs identify previous rows; use retained
contributions when reversing a previous calculation. A node that misses updates or sees
a new source must rebuild from current rows rather than assume that the latest delta
contains every missed change.

Borrowed views, row spans, and batch descriptors are valid during the invocation and
before mutation replaces their storage. Copy an owning scalar snapshot to retain values
beyond that lifetime.

## Publishing rows

Native outputs own their cells. Column outputs add or update nullable cells by row ID;
frame outputs add rows and patch fields:

```cpp
auto id = output.add(10.0);  // Out<TSC<double>>
output.set(id, 11.0);
output.erase(id);
```

`clear()` removes current rows while keeping the output valid. `invalidate()` removes
its current value. `apply(literal)` applies the same row literal syntax used by
`run_graph` inputs. Frame keys enforce uniqueness and cannot be changed by a patch.

## Copying and sampling

`output.copy(input)` copies the complete current value. Use it when taking a snapshot,
sampling after a gap, or rebuilding state. `copy_tick(input)` forwards a consecutive
update; it requires the consumer to have the preceding state. The standard
`sample(signal, value)` operator handles complete-value sampling, including frame and
column inputs. References follow the same [routing rules](../guide/advanced-graphs/references.md)
as built-in time series.

```cpp
auto snapshot = operators::sample(signal, quotes);
```

Queues accept native frame and column literals through `Queue::write`, so external
producers can submit the same updates as `run_graph`. Queue retention and elision follow
the [source and queue contracts](../reference/sources.md).

## Borrowed and computed outputs

Most custom nodes use native outputs. A view output borrows source storage and retains
its input connections:

`Out<TSC<double>, frame::view>` declares a column view;
`Out<TSF<Quotes>, frame::view>` declares a frame view.

Column views bind to a source column. Frame views use `bind_projection` to select
source fields or `bind_columns` to combine source fields with assigned columns.
Assigned columns must expose the source's exact row selection. A keyed view must retain
the source's complete key. Rebinding to a different row domain removes old rows and
adds new rows.

Computed outputs borrow source membership and own selected calculated cells. Use
`Out<TSC<T>, frame::computed<>>` for a column or
`Out<TSF<Schema>, frame::computed<"field">>` for selected frame fields. The
[expression API](frame-expressions.md) manages these outputs for ordinary graph code.

## Arrow interchange

Build with `BG_FRAME_BUILD_ARROW=ON`, provide an Arrow CMake package exporting
`Arrow::arrow_shared`, and link `bg::frame_arrow`. Include `<bg/frame/arrow.hpp>`.
The adapter converts scalar values; row IDs and time series deltas are not exported.

| Operation | Result |
| --- | --- |
| `borrow_arrow(column_view)` | Arrow chunked array borrowing compatible cell storage |
| `borrow_arrow(frame_view)` | Arrow table borrowing compatible column storage |
| `copy_to_arrow(view)` | Owning Arrow values, gathered into one dense chunk per column |
| `from_arrow<T>(array)` | Owning BG column scalar |
| `from_arrow<Schema>(table)` | Owning BG frame scalar with a declared schema |
| `from_arrow(table)` | Owning BG frame scalar with an inferred schema |

These functions return `arrow::Result`. Handle its error before using the result.
An imported value retains its Arrow owners. A borrowed export has the lifetime of the
BG view; use `copy_to_arrow` when the result must survive source mutation or leave the
node invocation.

```cpp
auto view = input.value_view();
auto exported = copy_to_arrow(view.value());
if (!exported.ok()) {
    throw std::runtime_error(exported.status().ToString());
}
auto table = *exported;
```

The adapter supports Boolean, int64, float64, text, and dictionary-encoded symbol
columns with nulls. Explicit frame schemas must match Arrow field names, order, and
types. Inferred schemas map text to `string` and text dictionaries to `symbol`.
