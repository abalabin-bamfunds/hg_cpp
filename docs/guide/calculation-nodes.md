# Write a calculation node

To apply a calculation that the library does not provide, define a node. Node
code receives current values and returns a result each time BG evaluates it.
Ordinary language control flow works on these values.

Prerequisites: [Graph functions](connect-calculations.md), [changing inputs](changing-values.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, test_graph

@node
def correct(reading: int) -> int:
    if reading < 0:
        return 0
    return reading + 2

@graph
def temperature(reading: int) -> int:
    return correct(reading)

print(test_graph(temperature, [18, -3, None, 19]))
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
#include <bg/testing/test_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::testing;
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
    const auto result = test_graph(graph(temperature), {18, -3, SKIP, 19});
    std::cout << std::boolalpha << result.equals({20, 0, SKIP, 21}) << '\n';
}
```

Output:

```text
true
```

C++ `graph(temperature)` gives the test a graph template, enabling literal
result comparisons with `SKIP`.
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
builds this example as `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

## Construction, evaluation, and execution

| Code | Runs when | Receives | Purpose |
| --- | --- | --- | --- |
| Graph function | During graph construction | Connections and fixed scalar arguments | Connect calculations |
| Node callback | When BG evaluates the node | Current values or input views | Calculate a result or perform an effect |
| `test_graph` | For a finite test run | Input update sequences | Record output updates |
| `run_graph` | For an application run | Graph construction arguments and run options | Execute sources, calculations, and sinks |

The node callback processes values. The graph function defines its dependencies. See [running graphs](run-graphs.md) for application execution.

## Values inside a node

The graph function connects `reading` to `correct`. The node callback receives
an integer, so its `if` tests the reading used by this evaluation. This differs
from [a conditional in graph code](conditions.md), which needs a graph operation
when its condition changes during execution.

A required input must have a current value before the node evaluates. A skipped
input position alone does not call this node. With several changing inputs, any
input update schedules it; it reads all current values after their dependencies
have evaluated. Simultaneous updates produce one evaluation.

Python `@node` uses plain value annotations. C++ `node(callable)` infers inputs
and output from the callable. A plain parameter can receive either a fixed
scalar argument or the current value of a connected time series.

Use [input views](input-views.md) when the calculation needs update information
as well as values. See [node declarations](../reference/nodes.md) for signatures.
