# Test update behavior

To check a calculation independently of its sources, pass update sequences to
`test_graph`. Include repeated values and skipped positions when their behavior
matters to downstream users.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph

@graph
def temperature(reading: int) -> int:
    return reading + 2

result = test_graph(temperature, [18, None, 18, 19])
assert result == [20, None, 20, 21]
print(result)
```

Output:

```text
[20, None, 20, 21]
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

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto temperature(wp<TS<i64>> reading) { return reading + 2; }

int main() {
    const auto result = test_graph(temperature, {18, SKIP, 18, 19});
    std::cout << std::boolalpha << result.equals({20, SKIP, 20, 21}) << '\n';
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

The first result establishes the current output. A skipped input supplies no
new event. An equal-valued reading still supplies an event and publishes an
equal output. Compare recorded updates, not only the final retained value.

For optional inputs, include absence, zero, invalidation, and recovery. For
collections, include adding, changing, invalidating, removing, and re-adding an
item. For state, repeat the run to verify that each runtime graph starts fresh.

Python root `None` cannot distinguish invalidation from no publication. Observe
`valid` in a second graph when that distinction matters. C++ comparisons can
use explicit `INVALID`. See [test reference](../reference/testing.md).
