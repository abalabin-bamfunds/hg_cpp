# Calculation nodes

If a graph needs a calculation that is specific to your application, write a node or
wrap an existing value callable. HG supplies its current inputs during evaluation
and publishes the returned result for downstream calculations.

## Value calculations

To reuse ordinary value code in a graph, wrap it with `node(callable)`. The callback
can keep its native `if` statements, loops, and helper calls because it receives values
during evaluation. Python also provides the decorator form `@node`; C++ infers the
inputs and result from the callable's signature.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph

def correct(reading: int) -> int:
    if reading < 0:
        return 0
    return reading + 2

correct_node = node(correct)

@graph
def temperature(reading: int) -> int:
    return correct_node(reading)

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

const auto correct = [](i64 reading) {
    if (reading < 0) {
        return i64{0};
    }
    return reading + 2;
};

const auto correct_node = node(correct);

int main() {
    const auto result = run_graph(
        [](auto reading) {
            return correct_node(reading);
        },
        {18, -3, SKIP, 19});
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

If your calculation needs to inspect every item in a complete snapshot, accept a
scalar collection and iterate inside the callback. The collection arrives as one
current value, so ordinary value code can process it in a single evaluation.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

If data needs to survive between evaluations, store an owning value in
[node state](../advanced-nodes/state.md). Callback arguments and borrowed views are
valid only for the current callback; graph code constructs connections and cannot
read their current values.
