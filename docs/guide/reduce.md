# Define a custom collection aggregate

To combine current items with an application-specific operation, define a
collection calculation. C++ `reduce` maintains a reducer graph; Python currently
uses a custom collection node for the same product calculation.

Prerequisites: [Collection reductions](collection-reductions.md), [input views](input-views.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, compute_node, graph, test_graph

@compute_node
def product(values: TSD[int, TS[int]]) -> TS[int]:
    result = 1
    for item in values.valid_values():
        result *= item.value
    return result

@graph
def combined(values: TSD[int, TS[int]]) -> int:
    return product(values)

print(test_graph(combined, [{1: 2, 2: 3}, {1: 4}, {2: REMOVE}]))
```

Output:

```text
[6, 12, 4]
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
#include <bg/reduce.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto multiply(wp<TS<i64>> left, wp<TS<i64>> right) {
    return (left * right).as<TS<i64>>();
}

auto combined(wp<TSD<i64, TS<i64>>> values) {
    return reduce(multiply, values, "default"_n = i64{1});
}

int main() {
    const auto result = test_graph(combined, {{{1, 2}, {2, 3}}, {{1, 4}}, {{2, REMOVE}}});
    std::cout << std::boolalpha << result.equals({6, 12, 4}) << '\n';
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

Both examples combine valid current items, replacing a contribution when its
item changes. A valid empty collection has product `1`. Python enumerates
valid item views in its callback. C++ forwards one valid item directly and uses
the supplied default when none are valid.

A C++ reducer must be associative and commutative. Item order and the reduction
tree are unspecified. Subtraction and division do not satisfy that contract.
The reducer's two inputs and result must have compatible scalar value types.
Do not rely on a particular number of reducer graph construction calls.

For standard totals, extrema, and means, use the
[built-in reductions](collection-reductions.md). See
[`reduce` reference](../reference/higher-order.md#reduce) for defaults, references,
and type constraints. Python does not expose the C++ reducer-graph API.
