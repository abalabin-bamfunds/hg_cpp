# Activity and dependency patterns

Input activity determines which updates schedule a calculation. Source selection
can also change which dependencies a calculation follows.

## Explicit triggers

`sample` reads a current value when a signal updates. The value remains a dependency,
but its updates alone do not publish a sample.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hg import graph, run_graph
from hg.operators import sample


@graph
def sampled_temperature(reading: int, signal: bool) -> int:
    calibrated = reading + 2
    return sample(signal, calibrated)


print(run_graph(
    sampled_temperature,
    [18, 19, None, 20, 21, None],
    [None, True, True, None, True, False],
))
```

Output:

```text
[None, 21, 21, None, 23, 23]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto sampled_temperature(wp<TS<i64>> reading, wp<TS<bool>> signal) {
    const auto calibrated = reading + 2;
    return sample(signal, calibrated);
}

int main() {
    const auto result = run_graph(
        sampled_temperature,
        {18, 19, SKIP, 20, 21, SKIP},
        {SKIP, true, true, SKIP, true, false});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 21, 21, None, 23, 23]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/hg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

A custom node expresses this with an active signal and a passive value input.
[Node input controls](../advanced-nodes/inputs.md#activity) defines those declarations.
Activity controls scheduling; validity controls whether evaluation is permitted.

## Selected dependencies

Dynamic item selection follows the selected item's current source. Updates to
unselected items do not become selected value updates. Membership changes can alter
availability independently of value updates.

The [reference example](references.md#selecting-a-changing-source) shows a key change,
a target update, and removal. [Iteration](../graphs/iteration.md) describes independent
per-key calculations.

## Dependencies between keys

`mesh` supports a per-key calculation that depends on another key's result. This
expresses relationships such as a component total depending on its parent total.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hg import TS, TSD, graph, mesh, node, run_graph

@node
def add_available(reading: int, dependency: int | None) -> int:
    return reading + (0 if dependency is None else dependency)

@graph
def branch(reading: int, dependency_key: int) -> int:
    return add_available(reading, mesh()[dependency_key])

@graph
def sensor_two(readings: TSD[int, TS[int]], dependencies: TSD[int, TS[int]]) -> int:
    return mesh(branch, readings, dependencies)[2]

print(run_graph(sensor_two, [{1: 1, 2: 10}, {1: 3, 2: 30}], [{2: 1}, None]))
```

Output:

```text
[11, 33]
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
#include <hg/mesh.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <optional>

using namespace hg;
using namespace hg::operators;
using namespace literal;

const auto add_available = node([](i64 reading, std::optional<i64> dependency) {
    return reading + dependency.value_or(0);
});

auto sensor_two(wp<TSD<i64, TS<i64>>> readings, wp<TSD<i64, TS<i64>>> dependencies) {
    return mesh([](auto reading, auto dependency_key) {
        return add_available(reading, mesh()[dependency_key]);
    }, readings, dependencies)[i64{2}].as<TS<i64>>();
}

int main() {
    const auto result = run_graph(sensor_two, {{{1, 1}, {2, 10}}, {{1, 3}, {2, 30}}}, {{{2, 1}}, SKIP});
    std::cout << result << '\n';
}
```

Output:

```text
[11, 33]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/hg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Each requested key owns a graph instance. Dependency requests establish evaluation
order. Recursive cycles are errors; the dependency graph must be acyclic.
The [mesh reference](../../reference/higher-order.md#mesh) specifies branch selection,
output access, and instance lifecycle.

## Feedback loops

Feedback sends an output change to a later evaluation time. A handle lets the graph
connect a loop before the calculation that supplies its input has been wired.

This loop starts from `1`, advances to `3`, and then stops publishing.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hg import Feedback, TS, graph, node, run_graph

@node
def advance(seed: int | None, previous: int | None) -> int:
    if previous is None:
        return seed
    if previous < 3:
        return previous + 1
    return None

@graph
def bounded(seed: TS[int]) -> TS[int]:
    loop = Feedback(TS[int], name="loop")
    loop.bind(advance(seed, loop.output))
    return loop.output

print(run_graph(bounded, [1]))
```

Output:

```text
[None, 1, 2, 3]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/feedback.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <optional>

using namespace hg;

const auto advance = node([](std::optional<i64> seed, std::optional<i64> previous) -> std::optional<i64> {
    if (!previous) {
        return seed;
    }
    if (*previous < 3) {
        return *previous + 1;
    }
    return std::nullopt;
});

auto bounded(wp<TS<i64>> seed) {
    auto loop = feedback<TS<i64>>("loop");
    loop = advance(seed, loop);
    return static_cast<wp<TS<i64>>>(loop);
}

int main() {
    std::cout << run_graph(bounded, {1}) << '\n';
}
```

Output:

```text
[None, 1, 2, 3]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/hg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The first result is invalid because feedback has not replayed an input yet. Each
publication schedules the next iteration. Returning `None` in Python or `std::nullopt`
in C++ leaves the node output unchanged and ends this loop. See the
[feedback contract](../../reference/sources.md#feedback) for types, timing, and lifetime.
