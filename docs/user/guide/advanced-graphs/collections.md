# Time series collections

Use independently changing items when sources publish partial updates or consumers
need dependencies on individual items. A scalar collection instead replaces its
complete value on each update.

| Shape | Type | Item identity |
| --- | --- | --- |
| List | `TSL` | Position |
| Bundle | `TSB` | Declared field name |
| Dictionary | `TSD` | Scalar key |
| Set value with membership changes | `TSS` | Scalar member |

## Lists

A fixed `TSL` holds a declared number of time series. Each position has its own
current value and update information.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, TSL, graph, run_graph

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/time_series/tsl/tsl.hpp>

using namespace bg;
using namespace bg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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

A bundle has named items whose time series types can differ. Its schema fixes the
field names and types.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TSB, graph, schema, symbol, run_graph
from bg.operators import getattr

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/time_series/tsb/tsb.hpp>

using namespace bg;
using namespace bg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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

A `TSD` maintains a changing set of keys and a separate time series for each key.
A supplied mapping is a delta: omitted keys remain unchanged.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, run_graph

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/time_series/tsd/tsd_type.hpp>

using namespace bg;
using namespace bg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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

## Sets

A `TSS` stores scalar members and exposes membership changes. Python uses `Removed`
for removal; C++ literals use `REMOVED`.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import Removed, TSS, graph, run_graph
from bg.operators import contains_

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/time_series/tss/tss_type.hpp>

using namespace bg;
using namespace bg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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

Collection root validity and item validity are separate. Ordinary reductions require
valid immediate items. An optional reduction ignores unavailable items.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import INVALID, TS, TSD, graph, opt, run_graph
from bg.operators import sum_

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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
