# Iteration

Iteration applies a calculation to several items. A fixed loop constructs a fixed
set of connections. `map` maintains separate graph instances for current keys.

## Fixed repetition

Native loops run during graph construction when the items are fixed scalar values.
Each iteration can add calculations to the graph.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import const_

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The loop executes once during construction. Its iteration count does not change
when graph inputs update.

## Per-key calculations with map

`map(graph, values)` applies a graph independently to each key. Updating one item
updates that key's calculation. Adding a key starts an instance; removing it stops
that instance.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, map, run_graph

@graph
def calibrate(reading: int) -> int:
    return reading + 2

@graph
def sensor_one(readings: TSD[int, TS[int]]) -> int:
    return map(calibrate, readings)[1]

print(run_graph(sensor_one, [{1: 18, 2: 30}, {2: 31}, {1: REMOVE}, {1: 19}]))
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
#include <bg/library.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/map.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto calibrate(wp<TS<i64>> reading) { return (reading + 2).as<TS<i64>>(); }

auto sensor_one(wp<TSD<i64, TS<i64>>> readings) {
    return map(calibrate, readings)[i64{1}].as<TS<i64>>();
}

int main() {
    const auto result = run_graph(graph(sensor_one), {{{1, 18}, {2, 30}}, {{2, 31}}, {{1, REMOVE}}, {{1, 19}}});
    std::cout << result << '\n';
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

`TSD[int, TS[int]]` in Python and `TSD<i64, TS<i64>>` in C++ describe keyed inputs
whose integer items update independently. Here, each input mapping supplies item
updates. The [collection chapter](../advanced-graphs/collections.md#dictionaries)
defines the type and update format in detail.

The example selects sensor 1 from the mapped results. An update to sensor 2 does not
recalculate sensor 1. Re-adding sensor 1 creates a fresh instance.

## Shared and mapped arguments

A scalar argument is fixed for all instances. `pass_through(value)` supplies one
complete changing input to every instance. An unwrapped dictionary argument is
mapped by key.


<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, TSD, graph, map, pass_through, run_graph

@graph
def calibrate(reading: int, offset: int) -> int:
    return reading + offset

@graph
def first(readings: TSD[int, TS[int]], offset: int) -> int:
    return map(calibrate, readings, pass_through(offset))[1]

print(run_graph(first, [{1: 10, 2: 20}, {2: 21}, None], [2, None, 3]))
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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto calibrate(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return (reading + offset).as<TS<i64>>();
}

auto first(wp<TSD<i64, TS<i64>>> readings, wp<TS<i64>> offset) {
    return map(calibrate, readings, pass_through(offset))[i64{1}].as<TS<i64>>();
}

int main() {
    const auto result = run_graph(first, {{{1, 10}, {2, 20}}, {{2, 21}}, SKIP}, {2, SKIP, 3});
    std::cout << result << '\n';
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

An offset update reaches every instance. A sensor 2 update leaves sensor 1 unchanged.

With several mapped dictionaries, the default key set is their union. Missing items
can leave an instance's required inputs unavailable. `__key_set_arg__` selects one
argument's keys; `__key_arg__` passes the current key into the inner graph.
The [map reference](../../reference/higher-order.md#map) specifies these options.

## Independent state

Each key owns its calculation state. Removing a key discards that state; re-adding it
starts from initialization. [Per-sensor totals](../../how-to/per-sensor-totals.md)
shows the effect with running sums.

Use [aggregation](aggregation.md) to combine items. Use
[`mesh`](../advanced-graphs/dependencies.md#dependencies-between-keys) when one key
needs another key's result.
