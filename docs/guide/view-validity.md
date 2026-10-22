# Check validity in node code

To make a calculation depend on input availability, accept an optional input.
Optional input views expose validity even when no value is available. Plain
optional inputs remain sufficient when only an absent value needs a fallback.

Prerequisites: [Input views](input-views.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import INVALID, TS, compute_node, graph, test_graph

@compute_node(valid=("reading",))
def uses_offset(reading: TS[int], offset: TS[int]) -> TS[bool]:
    return offset.valid

@graph
def offset_available(reading: int, offset: int) -> bool:
    return uses_offset(reading, offset)

print(test_graph(offset_available, [18, None, None, None], [None, 0, INVALID, 3]))
```

Output:

```text
[False, True, False, True]
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

const auto uses_offset = node([](In<TS<i64>> reading, In<TS<i64>, optional> offset) {
    return offset.valid();
});

auto offset_available(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return uses_offset(reading, offset);
}

int main() {
    std::cout << std::boolalpha << (test_graph(offset_available, {18, SKIP, SKIP, SKIP}, {SKIP, 0, INVALID, 3})
        .equals({false, true, false, true})) << '\n';
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

The reading remains valid after its first update. Offset updates and invalidation
then trigger the callback. A zero offset is available; absence is a separate state.

Python's `valid=("reading",)` requires only the reading to have a value;
`offset` is optional. Both inputs remain active. C++ adds `optional` to `In<>`
and uses `valid()`. Use `.value` in Python or `*offset` in C++ only after that
check succeeds. [Plain optional inputs](optional-inputs.md) receive `None` or
`std::nullopt` instead of a view.

Use the graph operator [`valid`](../reference/library-selection.md#valid)
when only an availability signal is needed. [Node declarations](../reference/nodes.md#input-activity-and-validity)
specify Python's `active`, `valid`, and `all_valid` options.

Required inputs still gate evaluation. Optional inputs may schedule evaluation
when they change, but the callback must check whether their current value exists.
Reading an invalid input has no value to return.

For collections, root validity does not imply that every item is valid. See
[collection input views](../reference/input-views.md#collection-views) and
[`all_valid`](../reference/library-selection.md#all_valid).
