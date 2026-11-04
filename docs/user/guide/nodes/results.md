# Results and evaluation

When a calculation needs several inputs, HG can wait until they are all available.
Once it evaluates, the node can publish a result or keep its previous output, which
is useful when a reading should be rejected without discarding the last good value.

## Required inputs

If your calculation needs every input, the default required-input behavior handles
the wait: each changing input must have a current value before the node evaluates.
Updates at the same time produce one evaluation with all current values, after the
node's dependencies have evaluated.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph


@node
def calibrated_temperature(reading: int, offset: int) -> int:
    return reading + offset


@graph
def display_temperature(reading: int, offset: int) -> int:
    return calibrated_temperature(reading, offset)


print(run_graph(
    display_temperature,
    [18, None, 19, None],
    [None, 2, None, 3],
))
```

Output:

```text
[None, 20, 21, 22]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <hg/node.hpp>

#include <iostream>

using namespace hg;
using namespace literal;

const auto calibrated_temperature = node([](i64 reading, i64 offset) {
    return reading + offset;
});

int main() {
    const auto result = run_graph(
        [](auto reading, auto offset) {
            return calibrated_temperature(reading, offset);
        },
        {18, SKIP, 19, SKIP},
        {SKIP, 2, SKIP, 3});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 20, 21, 22]
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

The first reading produces no result because the offset is not available yet.
The next offset update permits the calculation using that retained reading.
[Optional inputs](../advanced-nodes/inputs.md#optional-inputs) permit evaluation
without every input value.
The [validity and activity guide](../advanced-nodes/inputs.md) also covers `all_valid`
for complete collections and active/passive inputs for selecting evaluation triggers.

## Skipping publication

To reject a reading while keeping the last published result, return `None` in Python
or a disengaged `std::optional<T>` in C++. The node still processes the input, but
leaves its output unchanged.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <optional>

using namespace hg;
using namespace hg::operators;
using namespace literal;

const auto accepted = node([](i64 reading) -> std::optional<i64> {
    if (reading < 0) {
        return std::nullopt;
    }
    return reading + 2;
});

int main() {
    const auto result = run_graph(
        [](auto reading) {
            return accepted(reading);
        },
        {18, -3, 0, -1, 19});
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

If an input makes a calculation impossible, the node can raise an exception or apply
an explicit policy such as skipping publication. An exception ends evaluation and
returns control to the runner's caller; [error handling](../../how-to/errors.md#evaluation-errors)
shows both approaches. Value selection in graph code does not prevent unselected
branch calculations from evaluating.

Nodes needing update information use [input views](../advanced-nodes/inputs.md).
Nodes performing application effects use the same input rules and return no graph
output; see [sources and sinks](../execution/running.md).
