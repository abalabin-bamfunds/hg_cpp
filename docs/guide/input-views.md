# Read values and update information

To identify which input updated during a calculation, use input views. A view
provides the current value and information about its update. Plain value
parameters remain sufficient when the calculation only needs values.

Prerequisites: [Calculation nodes](calculation-nodes.md), [validity](validity.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, compute_node, graph, test_graph

@compute_node
def reading_changed(reading: TS[int], offset: TS[int]) -> TS[bool]:
    return reading.modified

@graph
def changes(reading: int, offset: int) -> bool:
    return reading_changed(reading, offset)

print(test_graph(changes, [18, None, 18, 19], [2, 3, None, 4]))
```

Output:

```text
[True, False, True, True]
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

const auto reading_changed = node([](In<TS<i64>> reading, In<TS<i64>> offset) {
    return reading.modified();
});

auto changes(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return reading_changed(reading, offset);
}

int main() {
    const auto result = test_graph(changes, {18, SKIP, 18, 19}, {2, 3, SKIP, 4});
    std::cout << std::boolalpha << result.equals({true, false, true, true}) << '\n';
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

Python `@compute_node` passes a view for each `TS[...]` parameter. Read a value
with `.value` and query an update with `.modified`. Its plain `int` parameters
are fixed scalar arguments. In C++, `In<TS<i64>>` requests a view; `*reading`
reads its value and `reading.modified()` queries its update.

At step 2, only the offset updates, so `reading.modified` is false. At step 3,
an equal-valued reading is still modified. At step 4, both inputs are modified
in the same evaluation.

`modified` identifies a valid update in the current evaluation. It is not a
general invalidation detector. Read an optional input's value only after
[checking validity](view-validity.md).

## View lifetime

Input views, their collection views, and iterators are valid only during the
callback. Keep an owning value in [node state](state.md) when it must survive
a callback. Do not retain a view.

[`Input views`](../reference/input-views.md) lists the properties and collection
access methods. Update timestamps are covered with [evaluation time](time.md).
