# Measure graph evaluation

To measure graph work, attach an evaluation profiler recorder to a run. Query
the profiler after execution. A recorder belongs to one graph run.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import EvaluationProfiler, graph, test_graph

@graph
def temperature(reading: int) -> int:
    return reading + 2

profiler = EvaluationProfiler()
test_graph(temperature, [18], recorder=profiler.recorder())
print(profiler.outer_totals()["cycles"])
```

Output:

```text
1
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
#include <bg/evaluation_profiler.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto temperature(wp<TS<i64>> reading) { return reading + 2; }

bool measured() {
    auto profiler = EvaluationProfiler{};
    test_graph(temperature, GraphRunOptions{.recorder = profiler.recorder()}, {18});
    return profiler.root_graph_id().has_value();
}

int main() {
    std::cout << std::boolalpha << (measured()) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [first program's CMake project](../guide/getting-started.md#write-and-run-the-example)
builds this example as `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

`report()` formats outer graph timing. Use graph metadata and node period queries
to identify which calculations account for work. Configure period lengths and
retention before a run when long-running graphs need bounded retained data.

A profile measures the selected execution mode. Simulation advances event time
without wall-clock waits; elapsed computation and evaluation timestamps answer
different questions. Realtime profiles also expose outer-cycle work under its
delivery schedule. Keep profiling configuration consistent when comparing runs.
See [execution reference](../reference/execution.md#profiling).
