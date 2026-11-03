# Collection inputs and publication

Collection views expose both current items and the changes in this evaluation.
They allow a node to process partial updates without rebuilding complete values.

## Current items

Current-item traversal computes a result from the collection's present state.
Validity-aware traversal omits unavailable values.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import REMOVE, TS, TSD, compute_node, graph, run_graph

@compute_node
def product(values: TSD[int, TS[int]]) -> TS[int]:
    result = 1
    for item in values.valid_values():
        result *= item.value
    return result

@graph
def combined(values: TSD[int, TS[int]]) -> int:
    return product(values)

print(run_graph(combined, [{1: 2, 2: 3}, {1: 4}, {2: REMOVE}]))
```

Output:

```text
[6, 12, 4]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/reduce.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>

using namespace hg;
using namespace hg::literals;
using namespace hg::operators;
using namespace literal;

const auto product = node([](In<TSD<i64, TS<i64>>> values) {
    auto result = i64{1};
    values.for_each_value([&](const auto item) {
        if (item.valid()) {
            result *= *item;
        }
    });
    return result;
});

auto combined(wp<TSD<i64, TS<i64>>> values) {
    return product(values);
}

int main() {
    const auto result = run_graph(combined, {{{1, 2}, {2, 3}}, {{1, 4}}, {{2, REMOVE}}});
    std::cout << result << '\n';
}
```

Output:

```text
[6, 12, 4]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Python `valid_values()` yields valid child input views. C++ `for_each_value` visits
each current item; the callback checks validity before reading its value. Both nodes
recompute the product from the current valid items.

## Change information

Dictionary addition, item modification, invalidation, and key removal are distinct
changes. Set updates expose added and removed members. Omitted items retain their
current state.

| Input | Python traversal | C++ access |
| --- | --- | --- |
| TSD current valid items | `valid_items()` / `valid_values()` | Key/value traversal and child views |
| TSD additions and removals | `added_items()` / `removed_items()` | Added/removed key traversal |
| TSD updates excluding additions | `updated_items()` | `for_each_updated_key_value` |
| TSD additions and updates | `modified_items()` | `for_each_modified_key_value` |
| TSS membership changes | `added()` / `removed()` | Added/removed member traversal |

The [input-view reference](../../reference/input-views.md#collection-views) specifies
the methods for each shape. A calculation that uses only available readings can request
a `valid_items` input view. It exposes an item becoming valid as an addition and an
item becoming invalid as a removal, so the same delta logic handles both.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from typing import Annotated
from hgraph import INVALID, REMOVE, TS, TSD, compute_node, graph, run_graph, valid_items
from hgraph.operators import sum_

@compute_node
def adjust(readings: Annotated[TSD[int, TS[int]], valid_items]) -> TSD[int, TS[int]]:
    delta = {key: REMOVE for key in readings.removed_keys()}
    for key in readings.modified_keys():
        delta[key] = readings[key].value + 2
    return delta

@graph
def total(readings: TSD[int, TS[int]]) -> int:
    return sum_(adjust(valid_items(readings)))

print(run_graph(total, [{1: 10, 2: 20}, {1: 11}, {2: INVALID}, {2: REMOVE}]))
```

Output:

```text
[34, 35, 13, None]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

const auto adjust = node([](In<TSD<i64, TS<i64>>, valid_items> readings, Out<TSD<i64, TS<i64>>> output) {
    readings.for_each_modified([&](const auto& key) { output[key].set(*readings[key] + 2); });
    readings.for_each_removed([&](const auto& key) { output.erase(key); });
});

auto total(wp<TSD<i64, TS<i64>>> readings) {
    return sum_(adjust(valid_items(readings)));
}

int main() {
    const auto result = run_graph(total, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, INVALID}}, {{2, REMOVE}}});
    std::cout << result << '\n';
}
```

Output:

```text
[34, 35, 13, None]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

When key `2` becomes invalid, the view reports its removal and the adjustment node
removes it from the output. The sum becomes `13`. Removing the already invalid key
produces no further output update.

## Publication

A collection node can publish only the rows or keys it changed. Omitted items retain
their current values, so the node does not need to reconstruct the complete output.
For a dictionary output, the following delta updates key `1` and removes key `2`:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
return {1: 12, 2: REMOVE}
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
output[i64{1}].set(i64{12});
output.erase(i64{2});
```
:::
::::

The [publication reference](../../reference/outputs.md) specifies returned values and
explicit outputs; [literal forms](../../reference/literals.md) specify collection deltas.

## Explicit C++ outputs

`Out<TS<T>>` provides `set(value)` and `invalidate()`. Collection outputs provide child
outputs and removal operations. Python publication uses returned deltas; its injected
`Output` view is read-only.

The [state example](state.md#retained-state) declares `In`, `State`, and `Out` fields in
a C++ aggregate. [Output reference](../../reference/outputs.md) specifies mutation,
copying complete values, copying updates, and publication constraints.
