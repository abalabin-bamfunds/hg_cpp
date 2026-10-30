# Results and evaluation

A node publishes its returned value. Its output retains that value until another
publication or invalidation.

## Required inputs

By default, each changing input must have a current value before evaluation. An
input update schedules the node after its dependencies evaluate. Simultaneous input
updates produce one evaluation with all current values.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph


@graph
def calibrated_temperature(reading: int, offset: int) -> int:
    return reading + offset


@graph
def display_temperature(reading: int, offset: int) -> int:
    calibrated = calibrated_temperature(reading, offset)
    return calibrated * 10


print(run_graph(
    display_temperature,
    [18, None, 19, None],
    [None, 2, None, 3],
))
```

Output:

```text
[None, 200, 210, 220]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace literal;

auto calibrated_temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return reading + offset;
}

auto display_temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    const auto calibrated = calibrated_temperature(reading, offset);
    return calibrated * 10;
}

int main() {
    const auto result = run_graph(
        display_temperature,
        {18, SKIP, 19, SKIP},
        {SKIP, 2, SKIP, 3});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 200, 210, 220]
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

The first reading produces no result because the offset is not available yet.
The next offset update permits the calculation using that retained reading.
[Optional inputs](../advanced-nodes/inputs.md#optional-inputs) permit evaluation
without every input value.

## Skipping publication

A calculation can accept an input without publishing a new result. Python returns
`None`; a C++ callable returns a disengaged `std::optional<T>`.

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
    std::cout << result << '\n';
}
```

Output:

```text
[20, None, 2, None, 21]
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

The negative reading leaves the output unchanged. Zero is a valid reading and
publishes the corrected value. Skipping publication preserves both the current value
and its availability.

## Errors

An exception fails the evaluation and returns control to the runner's caller.
[Error handling](../../how-to/errors.md#evaluation-errors) shows a failing node and a
publication rule that excludes invalid operations. Value selection in graph code
does not prevent unselected branch calculations from evaluating.

Nodes needing update information use [input views](../advanced-nodes/inputs.md).
Nodes performing application effects use the same input rules and return no graph
output; see [sources and sinks](../execution/running.md).
