# Summarize current items

To calculate a total across the sensors currently present, use `sum_`. It reads
the collection's current items. [`sums`](running-calculations.md) instead adds
successive updates to one value across time.

Prerequisites: [Dictionaries](dictionaries.md), [running calculations](running-calculations.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, test_graph
from bg.operators import sum_

@graph
def total(readings: TSD[int, TS[int]]) -> int:
    return sum_(readings)

print(test_graph(total, [{1: 18, 2: 20}, {1: 19}, {2: REMOVE}]))
```

Output:

```text
[38, 39, 19]
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

auto total(wp<TSD<i64, TS<i64>>> readings) {
    return sum_(readings);
}

int main() {
    std::cout << std::boolalpha << (test_graph(total, {{{1, 18}, {2, 20}}, {{1, 19}}, {{2, REMOVE}}}).equals({38, 39, 19})) << '\n';
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

Updating sensor 1 replaces its contribution. Removing sensor 2 removes its
contribution. Equal totals can still publish on separate input updates.

`min_`, `max_`, and `mean` operate on current numeric items too. `mean` returns
a floating-point result. A valid empty collection has sum zero; the other
reductions have no result.

## Include available items

Ordinary reductions require valid immediate items. `sum_(opt(readings))` skips
invalid items instead; it can produce zero when none contribute. This applies
to the immediate items, not recursively to every nested collection.

These reductions accept numeric fixed TSL, TSD, and TSS inputs. They do not
schedule themselves at graph start; a collection update triggers them.
For a custom associative and commutative combination, use
[`reduce`](reduce.md). See [collection reference](../reference/library-collections.md).
