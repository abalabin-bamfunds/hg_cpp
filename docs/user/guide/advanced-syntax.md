# Advanced syntax at a glance

When a calculation needs more than current values, explicit time series declarations
let it work with independent item updates, input availability, retained state, and
changing sources. This overview shows the syntax for those needs, with links to the
chapters that explain when and how to use each form.

## Time series declarations

If a connection needs a publication guarantee or an explicit collection or reference
shape, declare its time series type. The basic value form, `TS[int]` in Python or
`TS<i64>` in C++, describes an integer that can change over time:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, graph, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

If a source sends updates for individual keys rather than a complete snapshot, use
a time series dictionary. Each key keeps its current value until it is updated or
removed with `REMOVE`, so omitted keys do not need to be sent again.

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

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

To distinguish a new update from a retained value or check whether an input is
available, request an input view. Python `@compute_node` uses time series annotations
for views, while C++ requests them with `In<TS<T>>`.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, compute_node, graph, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

For a running total, peak, or other calculation that needs earlier observations,
use node state to retain data between evaluations. Each independent runtime node
initializes and owns its own state.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import State, graph, node, run_graph

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
#include <hg/library.hpp>
#include <hg/graph.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <algorithm>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

If a calculation needs to follow whichever source a key currently selects, use a
reference. It identifies the source time series, so changing the reference changes
which source an ordinary value input follows.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import REMOVE, TS, TSD, graph, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/time_series/tsd/tsd_type.hpp>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

See [references and dynamic binding](advanced-graphs/references.md) for target validity,
subscriptions, and source identity.
