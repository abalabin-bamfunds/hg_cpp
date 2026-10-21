# Group values by position

To keep a fixed number of independently changing values, use a time series
list, `TSL`. Each position has its own current value and update information.

Prerequisites: [Changing values](changing-values.md), [validity](validity.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, TSL, graph, test_graph

@graph
def first_sensor(readings: TSL[TS[int], 2]) -> int:
    return readings[0] + 2

print(test_graph(first_sensor, [{0: 18, 1: 30}, {1: 31}, {0: 19}]))
```

Output:

```text
[20, None, 21]
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
#include <bg/time_series/tsl/tsl.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto first_sensor(wp<TSL<TS<i64>, 2>> readings) {
    return readings[0] + 2;
}

int main() {
    const auto result = test_graph(first_sensor, {{{0, 18}, {1, 30}}, {{1, 31}}, {{0, 19}}});
    std::cout << std::boolalpha << result.equals({20, SKIP, 21}) << '\n';
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

`TSL[TS[int], 2]` or `TSL<TS<i64>, 2>` declares two positions containing integer
time series. Test input mappings update only the supplied indices. The second
step changes position 1; it does not recalculate the selected position 0.

A scalar list stored in one `TS` updates as one value. A TSL permits updates
and subscriptions to individual positions. Its declared size is fixed; an item
can be invalid without removing its position. Root validity and item validity
are separate. [Collection reductions](collection-reductions.md) can require all
items or use only available items.

Use a [bundle](bundles.md) for named positions with different types and a
[dictionary](dictionaries.md) when keys can appear and disappear.
[Type reference](../reference/types.md) lists collection declarations.
