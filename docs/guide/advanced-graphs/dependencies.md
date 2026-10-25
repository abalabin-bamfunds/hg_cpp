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
from bg import graph, run_graph
from bg.operators import sample


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
#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::operators;
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
    std::cout << std::boolalpha << result.equals({SKIP, 21, 21, SKIP, 23, 23}) << '\n';
}
```

Output:

```text
true
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
from bg import TS, TSD, graph, mesh, node, run_graph

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/mesh.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <optional>

using namespace bg;
using namespace bg::operators;
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
    std::cout << std::boolalpha << result.equals({11, 33}) << '\n';
}
```

Output:

```text
true
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

Each requested key owns a graph instance. Dependency requests establish evaluation
order. Recursive cycles are errors; the dependency graph must be acyclic.
The [mesh reference](../../reference/higher-order.md#mesh) specifies branch selection,
output access, and instance lifecycle.
