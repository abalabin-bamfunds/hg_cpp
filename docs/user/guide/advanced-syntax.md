# Advanced syntax at a glance

Explicit time series declarations express independent item updates, input-state
inspection, and publication constraints. These examples introduce the forms;
the linked chapters define their behavior.

## Time series declarations

`TS[int]` in Python and `TS<i64>` in C++ describe an integer time series.
Explicit declarations also support publication guarantees, collection items, and
reference targets. Ordinary integer connections use this form:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, graph, run_graph

@graph
def corrected(reading: TS[int]) -> TS[int]:
    return reading + 2

print(run_graph(corrected, [18, None, 19]))
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

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto corrected(wp<TS<i64>> reading) {
    return reading + 2;
}

int main() {
    const auto result = run_graph(corrected, {18, SKIP, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[20, None, 21]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

See [time series types](advanced-graphs/types.md) and
[guarantees](advanced-graphs/guarantees.md).

## Independently changing items

A time series dictionary updates individual items. Omitted keys retain their values;
`REMOVE` removes an item.

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

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

A scalar dictionary instead updates as one value. See
[time series collections](advanced-graphs/collections.md).

## Input views

An input view exposes current values and update information. Python `@compute_node`
uses time series annotations for views; C++ uses `In<TS<T>>`.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, compute_node, graph, run_graph

@compute_node
def reading_changed(reading: TS[int], offset: TS[int]) -> TS[bool]:
    return reading.modified

@graph
def changes(reading: int, offset: int) -> bool:
    return reading_changed(reading, offset)

print(run_graph(changes, [18, None, 18, 19], [2, 3, None, 4]))
```

Output:

```text
[True, False, True, True]
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

const auto reading_changed = node([](In<TS<i64>> reading, In<TS<i64>> offset) {
    return reading.modified();
});

auto changes(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return reading_changed(reading, offset);
}

int main() {
    const auto result = run_graph(changes, {18, SKIP, 18, 19}, {2, 3, SKIP, 4});
    std::cout << result << '\n';
}
```

Output:

```text
[true, false, true, true]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Optional views check validity before value access. Active inputs schedule evaluation;
passive inputs supply current values when another dependency triggers evaluation.
See [input controls](advanced-nodes/inputs.md).

## State

State belongs to a runtime node and survives between evaluations. It is initialized
separately for each independent instance.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import State, graph, node, run_graph

class Readings:
    def __init__(self):
        self.peak = 0

@node
def peak_above_zero(reading: int, state: State[Readings] = None) -> int:
    state.peak = max(state.peak, reading)
    return state.peak

@graph
def peak(reading: int) -> int:
    return peak_above_zero(reading)

print(run_graph(peak, [18, 17, 21, None, 19]))
```

Output:

```text
[18, 18, 21, None, 21]
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
#include <algorithm>

using namespace bg;
using namespace bg::operators;
using namespace literal;

struct Readings {
    i64 peak{};
};

struct PeakAboveZero {
    In<TS<i64>> reading;
    State<Readings> state;
    Out<TS<i64>> output;

    static constexpr bool deduplicate_wiring = false;

    void evaluate() const {
        state->peak = std::max(state->peak, *reading);
        output.set(state->peak);
    }
};

const auto peak_above_zero = node<PeakAboveZero>();

auto peak(wp<TS<i64>> reading) {
    return peak_above_zero(reading);
}

int main() {
    const auto result = run_graph(graph(peak), {18, 17, 21, SKIP, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[18, 18, 21, None, 21]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

See [state and lifecycle](advanced-nodes/state.md). The
[scheduling chapter](advanced-nodes/scheduling.md) adds clocks and scheduled evaluations.

## References

A reference identifies a time series. Changing the reference changes which source
an ordinary value input follows.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, run_graph

@graph
def selected(readings: TSD[int, TS[int]], sensor: int) -> int:
    return readings[sensor] + 2

print(run_graph(selected, [{1: 18, 2: 30}, {1: 19}, {2: 31}, {2: REMOVE}], [1, 2, None, None]))
```

Output:

```text
[20, 32, 33, None]
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

auto selected(wp<TSD<i64, TS<i64>>> readings, wp<TS<i64>> sensor) {
    return readings[sensor] + 2;
}

int main() {
    const auto result = run_graph(selected,
        {{{1, 18}, {2, 30}}, {{1, 19}}, {{2, 31}}, {{2, REMOVE}}}, {1, 2, SKIP, SKIP});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 32, 33, None]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

See [references and dynamic binding](advanced-graphs/references.md) for target validity,
subscriptions, and source identity.
