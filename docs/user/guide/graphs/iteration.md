# Iteration

If you need to apply the same transformation or computation to every item in a
collection, use `map`. It maintains a separate graph instance for each key, so items
can update independently and keep their own state. For a fixed set of calculations
known during construction, an ordinary loop is enough.

## Fixed repetition

When the items are fixed configuration, use a native loop to connect a calculation
for each one. The loop runs during graph construction, so the number of calculations
stays fixed even if their inputs change later.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import const_

@graph
def total() -> int:
    result = const_(0)
    for price in (10, 20, 30):
        result = result + price
    return result

print(run_graph(total))
```

Output:

```text
[60]
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

auto total() {
    auto result = const_(0).as<TS<i64>>();
    for (const auto price : {10, 20, 30}) {
        result = (result + price).as<TS<i64>>();
    }
    return result;
}

int main() {
    const auto result = run_graph(total);
    std::cout << result << '\n';
}
```

Output:

```text
[60]
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

## Per-key calculations with map

If readings arrive as complete dictionary snapshots, `map(graph, values)` applies
one calculation per key. Each snapshot supplies all current items. HG compares it
with the preceding snapshot, starts calculations for new keys, and stops them for
keys that disappeared. An unchanged item does not recalculate when its type supports
equality. The mapped calculation can be a graph function or a node template.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, map, run_graph

@graph
def calibrate(reading: int) -> int:
    return reading + 2

@graph
def sensor_one(readings: dict[int, int]) -> int:
    return map(calibrate, readings)[1]

print(run_graph(sensor_one, [
    {1: 18, 2: 30},
    {1: 18, 2: 31},
    {2: 31},
    {1: 19, 2: 31},
]))
```

Output:

```text
[20, None, None, 21]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;

int main() {
    const auto calibrate = node([](i64 reading) { return reading + 2; });
    std::cout << run_graph([&](auto readings) {
        return map(calibrate, readings)[1];
    }, input<TS<Dict<i64, i64>>>({
        {{1, 18}, {2, 30}},
        {{1, 18}, {2, 31}},
        {{2, 31}},
        {{1, 19}, {2, 31}},
    })) << '\n';
}
```

Output:

```text
[20, None, None, 21]
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

The result selects sensor 1 from the mapped calculations. Changing only sensor 2
leaves sensor 1 unchanged. Omitting sensor 1 from a later snapshot removes its
calculation; adding it back creates a fresh instance.

Python's `dict[int, int]` declares a complete scalar dictionary. C++ uses
`input<TS<Dict<i64, i64>>>` to distinguish whole-dictionary snapshots from item
updates; the graph callback can keep `auto` parameters. The input declaration selects
the collection shape, as in [scalar dictionaries](scalars.md#dictionaries).

When a source publishes only changed items, use a
[time series dictionary](../advanced-graphs/collections.md#mapping-item-updates)
to map those updates directly.

## Shared and mapped arguments

If every item needs the same changing configuration, wrap that argument in
`pass_through(value)` to send its current value to every instance. Scalar arguments
already supply a fixed value to all instances, while an unwrapped dictionary supplies
each instance with its matching item:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, map, pass_through, run_graph

@graph
def calibrate(reading: int, offset: int) -> int:
    return reading + offset

@graph
def first(readings: dict[int, int], offset: int) -> int:
    return map(calibrate, readings, pass_through(offset))[1]

print(run_graph(first, [{1: 10, 2: 20}, {1: 10, 2: 21}, None], [2, None, 3]))
```

Output:

```text
[12, None, 13]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

int main() {
    const auto calibrate = node([](i64 reading, i64 offset) { return reading + offset; });
    std::cout << run_graph([&](auto readings, auto offset) {
        return map(calibrate, readings, pass_through(offset))[1];
    }, input<TS<Dict<i64, i64>>>({
        {{1, 10}, {2, 20}},
        {{1, 10}, {2, 21}},
        SKIP,
    }), ticks({2, SKIP, 3})) << '\n';
}
```

Output:

```text
[12, None, 13]
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

An offset update reaches every instance. A sensor 2 update leaves sensor 1 unchanged.
C++ `ticks({...})` marks the offset argument as a sequence of updates alongside the
typed dictionary input.
The [map reference](../../reference/higher-order.md#map) describes mapping several
dictionaries and choosing the branch keys.

## Independent state

To keep a separate running total or other history for each item, put the stateful
calculation inside the mapped graph. Each key owns that state for its lifetime;
removing and re-adding the key starts it again from initialization.
[Per-sensor totals](../../how-to/per-sensor-totals.md) shows this with running sums.

Use [aggregation](aggregation.md) to combine items. Use
[`mesh`](../advanced-graphs/dependencies.md#dependencies-between-keys) when one key
needs another key's result.
