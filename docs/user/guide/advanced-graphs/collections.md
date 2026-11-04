# Time series collections

If a source sends partial updates, or a calculation needs to follow individual
items, use a time series collection. Its items keep their own values and update
information, so one item can change without replacing the complete collection.

| Shape | Type | Item identity |
| --- | --- | --- |
| List | `TSL` | Position |
| Bundle | `TSB` | Declared field name |
| Dictionary | `TSD` | Scalar key |
| Set value with membership changes | `TSS` | Scalar member |

## Lists

For values identified by position, such as a fixed group of sensor channels, use
a `TSL`. Its fixed-size form declares the number of positions, with a separate
current value and update information for each one.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, TSL, graph, run_graph

@graph
def first_sensor(readings: TSL[TS[int], 2]) -> int:
    return readings[0] + 2

print(run_graph(first_sensor, [{0: 18, 1: 30}, {1: 31}, {0: 19}]))
```

Output:

```text
[20, None, 21]
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
#include <hg/time_series/tsl/tsl.hpp>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto first_sensor(wp<TSL<TS<i64>, 2>> readings) {
    return readings[0] + 2;
}

int main() {
    const auto result = run_graph(first_sensor, {{{0, 18}, {1, 30}}, {{1, 31}}, {{0, 19}}});
    std::cout << result << '\n';
}
```

Output:

```text
[20, None, 21]
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

An update to position 1 does not update position 0. A position may be invalid while
remaining present. Sparse `TSL` types additionally permit changing occupied indices;
see [list types](../../reference/types.md#declarations).

## Bundles

If a record's named fields need to update independently, use a time series bundle.
Its schema fixes the field names and their time series types, which may differ
from one field to another.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TSB, graph, schema, symbol, run_graph
from hgraph.operators import getattr

@schema
class Sensor:
    reading: int
    enabled: bool

@graph
def temperature(sensor: TSB[Sensor]) -> int:
    return getattr(sensor, symbol("reading")) + 2

print(run_graph(temperature, [{"reading": 18, "enabled": True}, {"enabled": False}, {"reading": 19}]))
```

Output:

```text
[20, None, 21]
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
#include <hg/time_series/tsb/tsb.hpp>

using namespace hg;
using namespace hg::operators;
using namespace literal;

template <template <typename, auto...> typename Field>
struct Sensor {
    Field<TS<i64>> reading;
    Field<TS<bool>> enabled;
};

auto temperature(wp<TSB<Sensor>> sensor) {
    return getattr(sensor, symbol("reading")) + 2;
}

int main() {
    const auto result = run_graph(temperature,
        {{{"reading", 18}, {"enabled", true}}, {{"enabled", false}}, {{"reading", 19}}});
    std::cout << result << '\n';
}
```

Output:

```text
[20, None, 21]
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

The update to `enabled` leaves `reading` unchanged. Python scalar schema fields project
to time series fields in `TSB`; C++ declares the time series fields explicitly.
A scalar `Bundle` stores the fields in one value.

## Dictionaries

For items identified by keys that can arrive or disappear during a run, use a `TSD`.
Each key has a separate time series, and updates are deltas: omitted keys keep their
current state until another update changes or removes them.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import REMOVE, TS, TSD, graph, run_graph

@graph
def sensor_one(readings: TSD[int, TS[int]]) -> int:
    return readings[1] + 2

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/time_series/tsd/tsd_type.hpp>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto sensor_one(wp<TSD<i64, TS<i64>>> readings) {
    return readings[i64{1}] + 2;
}

int main() {
    const auto result = run_graph(sensor_one, {{{1, 18}, {2, 30}}, {{2, 31}}, {{1, REMOVE}}, {{1, 19}}});
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

`REMOVE` removes membership and the item's time series. `INVALID` leaves a key present
and removes its current value. [Iteration](../graphs/iteration.md) uses these keys to
maintain per-key graph instances.

### Mapping item updates

If a source publishes individual item changes, pass its `TSD` directly to `map`.
Each input mapping is a delta: omitted keys keep their values, and `REMOVE` deletes
a key and stops its graph instance. This avoids the full snapshot scan used by
[scalar dictionary mapping](../graphs/iteration.md#per-key-calculations-with-map).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import REMOVE, TS, TSD, graph, map, run_graph

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
#include <hg/library.hpp>
#include <hg/graph.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/map.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The update to sensor 2 leaves sensor 1 unchanged. Removing sensor 1 withdraws its
result; re-adding it creates a fresh instance. `map` accepts scalar dictionaries and
time series dictionaries in the same call. A scalar dictionary supplies snapshots,
while a time series dictionary supplies item updates.

## Sets

If you only need to track membership, use a `TSS` to report which scalar members
were added or removed. Python uses `Removed` for a removal, while C++ literals use
`REMOVED`.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import Removed, TSS, graph, run_graph
from hgraph.operators import contains_

@graph
def sensor_one_active(sensors: TSS[int]) -> bool:
    return contains_(sensors, 1)

print(run_graph(sensor_one_active, [{1, 2}, {3}, {Removed(1)}, {1}]))
```

Output:

```text
[True, None, False, True]
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
#include <hg/time_series/tss/tss_type.hpp>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto sensor_one_active(wp<TSS<i64>> sensors) {
    return contains_(sensors, i64{1});
}

int main() {
    const auto result = run_graph(sensor_one_active, {{1, 2}, {3}, {REMOVED(1)}, {1}});
    std::cout << result << '\n';
}
```

Output:

```text
[true, None, false, true]
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

A time series set is a set value with change information. Its members are scalars,
rather than individually valued time series.

## Aggregation and validity

When some items are unavailable, decide whether a total should wait for all of them
or use the available items. Ordinary reductions require valid immediate items;
optional reductions ignore unavailable ones. The collection root and its items
have separate validity, so a collection can be present with an invalid item.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import INVALID, TS, TSD, graph, opt, run_graph
from hgraph.operators import sum_

@graph
def total(values: TSD[int, TS[int]]) -> int:
    return sum_(opt(values))

print(run_graph(total, [{1: 10, 2: 20}, {2: INVALID}, {2: 5}]))
```

Output:

```text
[30, 10, 15]
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

auto total(wp<TSD<i64, TS<i64>>> values) {
    return sum_(opt(values));
}

int main() {
    const auto result = run_graph(total, {{{1, 10}, {2, 20}}, {{2, INVALID}}, {{2, 5}}});
    std::cout << result << '\n';
}
```

Output:

```text
[30, 10, 15]
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

The invalid item remains a member but stops contributing. Optional behavior applies
to immediate items; it does not recursively ignore invalid descendants.
[Collection node inputs](../advanced-nodes/collections.md) expose these states directly.
