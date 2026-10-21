# Track membership

To represent which sensors are active without attaching a value to each key,
use a time series set, `TSS`. Its updates add or remove members.

Prerequisites: [Dynamic keys](dictionaries.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import Removed, TSS, graph, test_graph
from bg.operators import contains_

@graph
def sensor_one_active(sensors: TSS[int]) -> bool:
    return contains_(sensors, 1)

print(test_graph(sensor_one_active, [{1, 2}, {3}, {Removed(1)}, {1}]))
```

Output:

```text
[True, None, False, True]
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
#include <bg/time_series/tss/tss_type.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto sensor_one_active(wp<TSS<i64>> sensors) {
    return contains_(sensors, i64{1});
}

int main() {
    std::cout << std::boolalpha << (test_graph(sensor_one_active, {{1, 2}, {3}, {REMOVED(1)}, {1}}).equals({true, SKIP, false, true})) << '\n';
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

A set delta adds ordinary members and removes marked members. Python also
accepts `SetDelta(added=..., removed=...)`. Repeated additions and removal of
absent members do not change membership. An empty set delta does not clear
existing membership.

The query subscribes to key 1. Adding key 3 alone produces no query update.
A time series key can select the queried member dynamically.

Two sets of the same key type support union, intersection, difference, and
symmetric difference with `|`, `&`, `-`, and `^`. Results publish actual membership
changes. Use [`opt`](../reference/library-collections.md#optional-set-operations)
when unavailable operands should act as empty sets.

[Collection reference](../reference/library-collections.md) specifies membership
queries, set equality, `is_empty`, and invalid-input behavior.
