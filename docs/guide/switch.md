# Select a graph instance

To run only the graph for a selected mode, use `switch_`. Unlike
[value selection](conditions.md), it starts and stops graph instances as the
selected case changes.

Prerequisites: [Conditionals](conditions.md), [node state](state.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, switch_, test_graph
from bg.operators import sums

@graph
def accumulated(reading: int) -> int:
    return sums(reading)

@graph
def current(reading: int) -> int:
    return reading

@graph
def temperature(mode: int, reading: int) -> int:
    return switch_(mode, {1: accumulated, 2: current}, reading)

print(test_graph(temperature, [1, None, 2, 1], [18, 19, 20, 21]))
```

Output:

```text
[18, 37, 20, 21]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <iostream>
#include <bg/switch.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto accumulated(wp<TS<i64>> reading) { return sums(reading); }
auto current(wp<TS<i64>> reading) { return reading; }

auto temperature(wp<TS<i64>> mode, wp<TS<i64>> reading) {
    return switch_(mode, cases{case_(i64{1}, accumulated), case_(i64{2}, current)}, reading)
        .as<TS<i64>>();
}

int main() {
    std::cout << std::boolalpha << (test_graph(temperature, {1, SKIP, 2, 1}, {18, 19, 20, 21}).equals({18, 37, 20, 21})) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
builds this example as `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

Mode 1 starts an accumulator. The next reading extends its history. Mode 2
stops that accumulator and follows the current reading. Returning to mode 1
creates a new accumulator, so its result is `21` rather than `58`.

Repeated keys selecting the same case keep its instance. Multiple keys can
select one case: Python uses a tuple key; C++ supplies multiple case keys.
`DEFAULT` handles unmatched or invalid keys. Without a default, an unmatched
or invalid key is an error.

The selected graph receives current argument values when it starts. Timers and
resources belong to that instance and stop with it. Graphs can be sources or
sinks as well as calculations. See [switch reference](../reference/higher-order.md#switch_).
