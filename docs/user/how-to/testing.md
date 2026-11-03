# Testing

If you want to check a calculation without connecting its live sources, pass update
sequences to `run_graph` and compare the captured results. Repeated values and gaps
between updates can matter to downstream calculations, so include them when they are
part of the behavior being tested.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph

@graph
def temperature(reading: int) -> int:
    return reading + 2

result = run_graph(temperature, [18, None, 18, 19])
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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <cassert>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto temperature(wp<TS<i64>> reading) { return reading + 2; }

int main() {
    const auto result = run_graph(temperature, {18, SKIP, 18, 19});
    assert(result.equals({20, SKIP, 20, 21}));
    std::cout << result << '\n';
}
```

Output:

```text
[20, None, 20, 21]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project)
builds this example as `main.cpp`, linked to `hg::library`.
:::
::::
:::::
::::::

The first result establishes the current output, which remains available when the
input skips a position. A repeated equal reading still supplies an event and publishes
an equal output, so the recorded updates contain more information than the final value.

For optional inputs, include absence, zero, invalidation, and recovery. For
collections, include adding, changing, invalidating, removing, and re-adding an
item. For state, repeat the run to verify that each runtime graph starts fresh.

Python root `None` cannot distinguish invalidation from no publication. Observe
`valid` in a second graph when that distinction matters. C++ comparisons can
use explicit `INVALID`. See [test reference](../reference/testing.md).
