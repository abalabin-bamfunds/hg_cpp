# Check validity in node code

To inspect an input that may not have a value, request an optional input view.
Check validity before reading the value. This supports decisions based on both
availability and update information.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, compute_node, graph, test_graph

@compute_node
def corrected(reading: TS[int], offset: TS[int] | None) -> TS[int]:
    return reading.value + (offset.value if offset.valid else 2)

@graph
def temperature(reading: int, offset: int) -> int:
    return corrected(reading, offset)

print(test_graph(temperature, [18, None, 19], [None, 0, None]))
```

Output:

```text
[20, 18, 19]
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

const auto corrected = node([](In<TS<i64>> reading, In<TS<i64>, optional> offset) {
    return *reading + (offset.valid() ? *offset : 2);
});

auto temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return corrected(reading, offset);
}

int main() {
    std::cout << std::boolalpha << (test_graph(temperature, {18, SKIP, 19}, {SKIP, 0, SKIP}).equals({20, 18, 19})) << '\n';
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

The optional Python annotation still provides a view when its input is invalid;
the callback does not receive `None`. This differs from an
[optional plain parameter](optional-inputs.md), which receives an absent value.
C++ adds `optional` to `In<>` and uses `valid()`.

Required inputs still gate evaluation. Optional inputs may schedule evaluation
when they change, but the callback must check whether their current value exists.
Reading an invalid input has no value to return.

For collections, root validity does not imply that every item is valid. See
[collection input views](../reference/input-views.md#collection-views) and
[`all_valid`](../reference/library-selection.md#all_valid).
