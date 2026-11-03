# Frame nodes and interchange

If your table calculation needs logic beyond the [column operations](frame-expressions.md),
write a custom node to read current rows, process changed rows, or publish row updates.
These interfaces also support copying snapshots and exchanging scalar data with Arrow.

## Reading current values and changes

To read rows in a custom node, accept `In<TSC<T>>` or `In<TSF<Schema>>`. These views
combine ordinary validity and modification queries with row access; a null cell
returns `std::optional`, while a missing row throws. Check optional cells before
reading their values.

| Read | Purpose |
| --- | --- |
| `size()`, `contains(id)` | Inspect current row membership |
| `value_view()` | Borrow a scalar view during this invocation |
| `for_each_row(callback)` | Visit current rows |
| `for_each_changed(callback)` | Visit added or modified rows in this cycle |
| `for_each_removed(callback)` | Visit removed row IDs |
| `has_row_id_remapping()` | Test whether surviving rows changed internal IDs |
| `for_each_row_id_remapping(callback)` | Visit `(old_id, new_id)` pairs |
| `row_id_remappings()` | Borrow the same pairs as a contiguous span |
| `cell(id)` on a column | Read one nullable cell |
| `cell<Index>(id)` on a frame | Read one nullable field |

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
#include <hg/frame/tsc.hpp>
#include <hg/frame/literal.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::frame;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages" -DHG_SOURCE_DIR=/path/to/hg
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

When a reference switches between keyed frames with the same ordered key names
and cell types, matching key tuples identify surviving rows. The sampled input
reports those rows as modified in every column. It reports an ID remapping only
when a surviving row's numeric ID changes. The frame and its column inputs share
these mappings. Unkeyed sources and sources with different key definitions report
replacement through removals and additions.

Nodes must process removed IDs in the previous ID domain, apply remappings
simultaneously, and then process additions and modifications in the current domain.
Mappings can swap IDs or target an ID occupied by a removed row. A node can also
discard the cached contributions of remapped rows and recompute them from the
reported modifications. The remapping query does not scan keys or rows; key
matching occurs once when constructing the sampled reference change.

Borrowed views, row spans, and batch descriptors are valid during the invocation and
before mutation replaces their storage. Copy an owning scalar snapshot to retain values
beyond that lifetime.

## Publishing rows

To maintain a table from a custom node, publish through its native output, which
owns the cells. Column outputs add or update nullable cells by row ID; frame outputs
add rows and patch fields:

```cpp
auto id = output.add(10.0);  // Out<TSC<double>>
output.set(id, 11.0);
output.erase(id);
```

To reset a table to an available empty result, use `clear()`; to withdraw its current
value, use `invalidate()`. For updates already expressed as row literals, `apply(literal)`
accepts the same syntax as `run_graph` inputs. Frame keys enforce uniqueness and cannot
be changed by a patch.

## Copying and sampling

If a consumer needs a snapshot after a gap or must rebuild its state, use
`output.copy(input)` to copy the complete current value. Use `copy_tick(input)` for
a consecutive update when the consumer already has the preceding state. The standard
`sample(signal, value)` operator handles complete-value sampling for frames and
columns, and references follow the ordinary [routing rules](../guide/advanced-graphs/references.md).

```cpp
auto snapshot = operators::sample(signal, quotes);
```

Queues accept native frame and column literals through `Queue::write`, so external
producers can submit the same updates as `run_graph`. Queue retention and elision follow
the [source and queue contracts](../reference/sources.md).

## Borrowed and computed outputs

If an output can reuse source storage, a view output avoids copying those cells
and keeps the input connections it borrows from. Native outputs remain the usual
choice when a custom node owns and updates its result:

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

To exchange scalar columns or frames with Arrow, build with `HG_FRAME_BUILD_ARROW=ON`,
provide an Arrow package exporting `Arrow::arrow_shared`, and link `hg::frame_arrow`.
Include `<hg/frame/arrow.hpp>` for the adapter, which converts scalar values rather
than row IDs or time series deltas.

| Operation | Result |
| --- | --- |
| `borrow_arrow(column_view)` | Arrow chunked array borrowing compatible cell storage |
| `borrow_arrow(frame_view)` | Arrow table borrowing compatible column storage |
| `copy_to_arrow(view)` | Owning Arrow values, gathered into one dense chunk per column |
| `from_arrow<T>(array)` | Owning HG column scalar |
| `from_arrow<Schema>(table)` | Owning HG frame scalar with a declared schema |
| `from_arrow(table)` | Owning HG frame scalar with an inferred schema |

These functions return `arrow::Result`. Handle its error before using the result.
An imported value retains its Arrow owners. A borrowed export has the lifetime of the
HG view; use `copy_to_arrow` when the result must survive source mutation or leave the
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
