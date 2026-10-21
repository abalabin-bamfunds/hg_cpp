# Run sources and sinks

To run a complete application graph, connect a source to a sink and call
`run_graph`. A source introduces values; a sink consumes values without
producing a graph output. Use `test_graph` when recorded output updates are needed.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph
from bg.operators import const_

@node
def display(temperature: int) -> None:
    print(temperature)

@graph
def application() -> None:
    display(const_(18) + 2)

run_graph(application)
```

Output:

```text
20
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
#include <bg/run_graph.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

const auto display = node([](i64 temperature) { std::cout << temperature << '\n'; });

auto application() {
    display(const_(18) + 2);
}

int main() {
    run_graph(application);
}
```

Output:

```text
20
```
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
builds this example as `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

The fixed source publishes once. The sink runs after the addition. The default
simulation run finishes when no work remains. `run_graph` does not return the
graph's output values; use sinks for application effects.

A sink has no output declaration: Python uses `-> None`, and a C++ callable
returns `void`. Its input rules are the same as a calculation node's rules.

## Choose execution behavior

Simulation advances to scheduled evaluation times without waiting for wall-clock
time. Realtime execution waits for input or timers until a run limit or stop
request ends the run. An unbounded realtime graph can remain idle indefinitely.

[`Running graphs`](../reference/execution.md) lists engine selection, start and
stop times, cancellation, service catalogues, and profiling options. Use a
[finite scheduled source](schedule.md) before introducing external producers.
