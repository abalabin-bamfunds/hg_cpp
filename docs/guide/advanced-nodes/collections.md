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
from bg import REMOVE, TS, TSD, compute_node, graph, run_graph

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/reduce.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::operators;
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
    std::cout << std::boolalpha << result.equals({6, 12, 4}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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
| TSD modified items | `modified_items()` | Modified child traversal |
| TSS membership changes | `added()` / `removed()` | Added/removed member traversal |

The [input-view reference](../../reference/input-views.md#collection-views) specifies
the methods for each shape. `valid_items` input views expose immediate item validity
changes as key additions and removals.


<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, compute_node, graph, run_graph
from bg.operators import sum_

@compute_node
def adjust(readings: TSD[int, TS[int]]) -> TSD[int, TS[int]]:
    delta = {key: REMOVE for key in readings.removed_keys()}
    for key in (*readings.added_keys(), *readings.modified_keys()):
        item = readings[key]
        if item.valid:
            delta[key] = item.value + 2
    return delta

@graph
def total(readings: TSD[int, TS[int]]) -> int:
    return sum_(adjust(readings))

print(run_graph(total, [{1: 10, 2: 20}, {1: 11}, {2: REMOVE}]))
```

Output:

```text
[34, 35, 13]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

const auto adjust = node([](In<TSD<i64, TS<i64>>> readings, Out<TSD<i64, TS<i64>>> output) {
    const auto publish = [&](const auto& key) {
        const auto item = readings[key];
        if (item.valid()) {
            output[key].set(*item + 2);
        } else {
            output[key].invalidate();
        }
    };
    readings.for_each_added(publish);
    readings.for_each_modified(publish);
    readings.for_each_removed([&](const auto& key) { output.erase(key); });
});

auto total(wp<TSD<i64, TS<i64>>> readings) {
    return sum_(adjust(readings));
}

int main() {
    const auto result = run_graph(total, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, REMOVE}}});
    std::cout << std::boolalpha << result.equals({34, 35, 13}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The adjustment node publishes only changed or removed keys. The downstream sum reads
its current output collection. Removing key `2` removes its adjusted value from the sum.

## Publication

Python nodes return values or collection deltas. A returned `None` leaves the output
unchanged. C++ nodes can return values or use an explicit `Out<>` field.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph

@node
def accepted(reading: int) -> int:
    if reading < 0:
        return None
    return reading + 2

@graph
def temperature(reading: int) -> int:
    return accepted(reading)

print(run_graph(temperature, [18, -3, 0, -1, 19]))
```

Output:

```text
[20, None, 2, None, 21]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <optional>

using namespace bg;
using namespace bg::operators;
using namespace literal;

const auto accepted = node([](i64 reading) -> std::optional<i64> {
    if (reading < 0) {
        return std::nullopt;
    }
    return reading + 2;
});

auto temperature(wp<TS<i64>> reading) {
    return accepted(reading);
}

int main() {
    const auto result = run_graph(graph(temperature), {18, -3, 0, -1, 19});
    std::cout << std::boolalpha << result.equals({20, SKIP, 2, SKIP, 21}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

`INVALID` removes the output's current value. A collection delta updates only supplied
items. `REMOVE` removes a dictionary key; omitted keys remain unchanged.
[Literal reference](../../reference/literals.md) specifies each delta format.

## Explicit C++ outputs

`Out<TS<T>>` provides `set(value)` and `invalidate()`. Collection outputs provide child
outputs and removal operations. Python publication uses returned deltas; its injected
`Output` view is read-only.

The [state example](state.md#retained-state) declares `In`, `State`, and `Out` fields in
a C++ aggregate. [Output reference](../../reference/outputs.md) specifies mutation,
copying complete values, copying updates, and publication constraints.
