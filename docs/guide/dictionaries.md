# Group values by dynamic key

To maintain one changing value per sensor as sensors appear and disappear,
use a time series dictionary, `TSD`. Keys select independently changing items.

Prerequisites: [Fixed lists](fixed-lists.md), [validity](validity.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, test_graph

@graph
def sensor_one(readings: TSD[int, TS[int]]) -> int:
    return readings[1] + 2

print(test_graph(sensor_one, [{1: 18, 2: 30}, {2: 31}, {1: REMOVE}, {1: 19}]))
```

Output:

```text
[20, None, None, 21]
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

auto sensor_one(wp<TSD<i64, TS<i64>>> readings) {
    return readings[i64{1}] + 2;
}

int main() {
    const auto result = test_graph(sensor_one, {{{1, 18}, {2, 30}}, {{2, 31}}, {{1, REMOVE}}, {{1, 19}}});
    std::cout << std::boolalpha << result.equals({20, SKIP, INVALID, 21}) << '\n';
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

Each input mapping is a delta. A new key adds an item; an existing key updates
that item; `REMOVE` removes the key and item. Omitted keys are unchanged.
An empty delta does not remove existing keys.

Removing key 1 makes the selected value unavailable. A later addition at key 1
makes it available again. A dynamic time series key can select a different item
during execution; selection then follows the new item's current value.

## Membership and availability

A present key can have an invalid item. `INVALID` removes an item's current
value while keeping its key; `REMOVE` removes the key. `contains_` tests key
membership. `valid_items` exposes only keys with valid immediate items to an
accepting input. The physical `key_set` still reports physical membership.

TSD roots do not support invalidation through test deltas. Clearing a dictionary
means removing its keys. Nested dictionaries and other collection items apply
their own delta rules recursively.

Use [map](map.md) for a separate calculation per key and
[collection reductions](collection-reductions.md) for one result across current
items. See [literal reference](../reference/literals.md) for nested deltas.
