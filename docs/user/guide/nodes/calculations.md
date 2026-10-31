# Calculation nodes

A node implements a calculation unavailable in the library. Node code receives
current values and can use ordinary language control flow and helper functions.

## Value calculations

Python `@node` declares plain value inputs and a result. C++ `node(callable)` infers
them from the callable's signature.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph

@node
def correct(reading: int) -> int:
    if reading < 0:
        return 0
    return reading + 2

@graph
def temperature(reading: int) -> int:
    return correct(reading)

print(run_graph(temperature, [18, -3, None, 19]))
```

Output:

```text
[20, 0, None, 21]
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

using namespace bg;
using namespace bg::operators;
using namespace literal;

const auto correct = node([](i64 reading) {
    if (reading < 0) {
        return i64{0};
    }
    return reading + 2;
});

auto temperature(wp<TS<i64>> reading) {
    return correct(reading);
}

int main() {
    const auto result = run_graph(graph(temperature), {18, -3, SKIP, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 0, None, 21]
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

The callback's `if` tests the integer value used in this evaluation. The surrounding
graph function connects the callback to its input.

| Code | Runs when | Receives |
| --- | --- | --- |
| Graph function | During construction | Connections and fixed arguments |
| Node callback | During evaluation | Current values or explicit input views |

A plain parameter can receive a fixed scalar or the current value of a connection.
A fixed argument remains unchanged for that node throughout the run.

## Scalar collections

A scalar collection arrives as one current value. Ordinary iteration can process it
inside the callback.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph

@node
def total(prices: tuple[int, ...]) -> int:
    return sum(prices)

@graph
def portfolio(prices: tuple[int, ...]) -> int:
    return total(prices)

print(run_graph(portfolio, [(10, 20), (12, 20), (12,)]))
```

Output:

```text
[30, 32, 12]
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

const auto total = node([](In<TS<List<i64>>> input) {
    const auto prices = input.value_view();
    auto result = i64{};
    for (auto index = std::size_t{}; index < prices.size(); ++index) {
        result += prices[index].value();
    }
    return result;
});

auto portfolio(wp<TS<List<i64>>> prices) {
    return total(prices);
}

int main() {
    const auto result = run_graph(portfolio, {{10, 20}, {12, 20}, {12}});
    std::cout << result << '\n';
}
```

Output:

```text
[30, 32, 12]
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

Python receives an immutable tuple. C++ `In<TS<List<i64>>>` receives the changing collection; `value_view()` provides a
borrowed scalar view. Each list item is read through its scalar interface. The callback recomputes
the total from the current complete list. Independently updating items use
[collection time series inputs](../advanced-nodes/collections.md).

## Callback lifetime

Arguments and borrowed views belong to the current callback. Store an owning value
in [node state](../advanced-nodes/state.md) when it must survive between evaluations.
Graph code cannot read current values while constructing connections.
