# Follow a changing source

To follow whichever sensor a changing key selects, select an item from a time
series dictionary. Selection returns a reference to the item, allowing
subsequent target updates to reach the calculation without copying each value.

Prerequisites: [Dictionary selection](dictionaries.md), [input views](input-views.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, test_graph

@graph
def selected(readings: TSD[int, TS[int]], sensor: int) -> int:
    return readings[sensor] + 2

print(test_graph(selected, [{1: 18, 2: 30}, {1: 19}, {2: 31}, {2: REMOVE}], [1, 2, None, None]))
```

Output:

```text
[20, 32, 33, None]
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
#include <bg/time_series/tsd/tsd_type.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto selected(wp<TSD<i64, TS<i64>>> readings, wp<TS<i64>> sensor) {
    return readings[sensor] + 2;
}

int main() {
    const auto result = test_graph(selected,
        {{{1, 18}, {2, 30}}, {{1, 19}}, {{2, 31}}, {{2, REMOVE}}}, {1, 2, SKIP, SKIP});
    std::cout << std::boolalpha << result.equals({20, 32, 33, INVALID}) << '\n';
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

At step 2, selection changes to sensor 2 and exposes its current value `30`.
At step 3, that sensor's update reaches the result. Removing sensor 2 makes the
result unavailable. Selection can later recover when the key has a valid item.

`REF[TS[int]]` in Python or `REF<TS<i64>>` in C++ explicitly declares a reference
to an integer time series. A reference has its own value and update time: its
value identifies a target time series. Target validity and target updates are
separate from the reference changing targets.

Ordinary value inputs follow references automatically. Code requesting an
explicit reference input receives the reference value instead. References do
not own target history. Do not retain callback views or references to removed
storage outside their supported lifetime.

Value selection, map outputs, switch outputs, and dictionary selection use
references. [Reference types](../reference/types.md#references) specifies
compatibility and validity. [Input views](../reference/input-views.md) distinguishes
reference-value access from dereferenced value access.
