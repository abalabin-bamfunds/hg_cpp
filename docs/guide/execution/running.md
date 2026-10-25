# Running applications

`run_graph` constructs and runs a graph. It can capture an output, execute sinks,
or do both. Simulation is the default engine.

## Sources and sinks

A source introduces values. A sink consumes values and performs an application
effect without producing a graph output.

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
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
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

A Python sink uses `-> None`; a C++ sink returns `void`. The fixed source publishes
once, the addition evaluates, and the sink prints `20`. With no work remaining,
the simulation ends. Outputless runs have no captured result.

## Capturing results

Graphs returning an output capture sparse events by default. Python returns
`GraphRunResult`; C++ returns a result supporting literal comparisons. Set
`capture_output=False` in Python or `GraphRunOptions::capture_output = false` in C++
to disable capture.

[Changing inputs](../graphs/updates.md) introduces finite input sequences. The
[result reference](../../reference/testing.md) covers timestamps, dense positions,
and large gaps.

## Simulation and real-time execution

Simulation advances directly to the next scheduled evaluation time. Realtime execution
waits for input or timers. Real-time graphs need a run limit or stop request when they
can remain idle indefinitely.

[Execution reference](../../reference/execution.md) lists engine selection, start times,
stop times, duration limits, cancellation, services, and profiling. The
[time patterns](time.md) use finite sources so each example terminates.
