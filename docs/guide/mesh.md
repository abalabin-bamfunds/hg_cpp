# Resolve dependencies between keys

To calculate a sensor result using another sensor's derived result, use `mesh`.
It gives each key a graph instance and evaluates dependency branches before the
branches that read them.

Prerequisites: [Map](map.md), [references](references.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, TSD, graph, mesh, node, test_graph

@node
def add_available(reading: int, dependency: int | None) -> int:
    return reading + (0 if dependency is None else dependency)

@graph
def branch(reading: int, dependency_key: int) -> int:
    return add_available(reading, mesh()[dependency_key])

@graph
def sensor_two(readings: TSD[int, TS[int]], dependencies: TSD[int, TS[int]]) -> int:
    return mesh(branch, readings, dependencies)[2]

print(test_graph(sensor_two, [{1: 1, 2: 10}, {1: 3, 2: 30}], [{2: 1}, None]))
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
#include <bg/testing/test_graph.hpp>
#include <iostream>
#include <bg/mesh.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <optional>

using namespace bg;
using namespace bg::testing;
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
    std::cout << std::boolalpha << (test_graph(sensor_two, {{{1, 1}, {2, 10}}, {{1, 3}, {2, 30}}}, {{{2, 1}}, SKIP}).equals({11, 33})) << '\n';
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

Branch 2 reads branch 1 through `mesh()[dependency_key]`. Branch 1 has no
dependency key; its dependency value is unavailable, so the node adds zero.
When both readings change, branch 1's new result is available before branch 2
completes its calculation.

A changing dependency key retargets the dependency. A requested key can create a
branch even when the normal mapped key set lacks it. That branch remains while
another branch needs it. Dependency cycles fail with a cycle diagnostic.

Mesh uses map's argument and key-selection rules. Name nested meshes when an
inner branch needs to refer to a particular outer mesh: `mesh("name")[key]`.
See [mesh reference](../reference/higher-order.md#mesh) for lifetimes and errors.
