# Activity and dependency patterns

A calculation may need to keep reading a value without publishing every change,
follow only a selected source, or depend on another key's result. Dependency
controls express those relationships and determine which updates cause work.

## Explicit triggers

A dashboard may need to publish on request while its source values keep changing.
`sample(signal, value)` reads the current value when the signal updates. The
[sampling example](../graphs/updates.md#signal-driven-publication) shows which updates
publish; [running summaries on request](../../how-to/requested-summary.md) combines
sampling with retained statistics.

A custom node expresses this with an active signal and a passive value input.
[Node input controls](../advanced-nodes/inputs.md#active-and-passive-inputs) gives the declarations.
Activity controls scheduling; validity controls whether evaluation is permitted.

## Selected dependencies

If only one item's updates should reach a consumer, select that item dynamically
rather than combining the complete collection. The consumer follows the selected
source; changes to other items do not become selected value updates, though
membership changes can affect the selected item's availability.

The [reference example](references.md#selecting-a-changing-source) shows a key change,
a target update, and removal. [Iteration](../graphs/iteration.md) describes independent
per-key calculations.

## Dependencies between keys

If a per-key calculation needs another key's result, use `mesh` to express that
dependency. This supports relationships such as a component total depending on
its parent total, while HG orders the calculations according to their dependencies.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, TSD, graph, mesh, node, run_graph

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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

To feed a result back into a later evaluation, use a feedback handle. It lets you
connect the loop before its supplying calculation has been wired, and sends each
output change to a later evaluation time.

This loop starts from `1`, advances to `3`, and then stops publishing.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import Feedback, TS, graph, node, run_graph

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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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
