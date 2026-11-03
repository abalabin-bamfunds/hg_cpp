# Running applications

To run a graph as an application, connect sources that supply values and sinks that
consume results, then call `run_graph`. Output capture remains available when you
want to inspect or test the run, and simulation can process finite inputs without
waiting for wall-clock time.

## Sources and sinks

If a graph needs to receive data or affect the application, use sources for incoming
values and sinks for actions such as printing or sending a result. A sink consumes
graph values without producing another graph output.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph
from hgraph.operators import const_

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

To inspect a graph's publications after a run, return an output from the graph and
keep the default output capture. Python returns `GraphRunResult`, while C++ returns
a result supporting literal comparisons. For runs that do not need a retained result,
set `capture_output=False` in Python or `GraphRunOptions::capture_output = false` in C++.

[Changing inputs](../graphs/updates.md) introduces finite input sequences. The
[result reference](../../reference/testing.md) covers timestamps, dense positions,
and large gaps.

## Simulation and realtime execution

For recorded inputs and deterministic time-based calculations, simulation advances
directly to the next scheduled evaluation time. Choose realtime execution when the
graph needs to wait for live inputs or timers, and supply a run limit or stop request
if it could otherwise remain idle indefinitely.

[Execution reference](../../reference/execution.md) lists engine selection, start times,
stop times, duration limits, cancellation, services, and profiling. The
[time patterns](time.md) use finite sources so each example terminates.
