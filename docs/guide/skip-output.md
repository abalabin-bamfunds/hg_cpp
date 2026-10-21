# Leave an output unchanged

To reject a reading without replacing the last accepted result, return no
output update. Python uses `None`; C++ uses a disengaged `std::optional` return.

Prerequisites: [Calculation nodes](calculation-nodes.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, test_graph

@node
def accepted(reading: int) -> int:
    if reading < 0:
        return None
    return reading + 2

@graph
def temperature(reading: int) -> int:
    return accepted(reading)

print(test_graph(temperature, [18, -3, 0, -1, 19]))
```

Output:

```text
[20, None, 2, None, 21]
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
#include <optional>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

const auto accepted = node([](i64 reading) -> std::optional<i64> {
    if (reading < 0) {
        return std::nullopt;
    }
    return reading + 2;
});

auto temperature(wp<TS<i64>> reading) {
    return accepted(reading);
}

int main() {
    std::cout << std::boolalpha << (test_graph(temperature, {18, -3, 0, -1, 19}).equals({20, SKIP, 2, SKIP, 21})) << '\n';
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

At step 2, the output remains `20`. At step 4, it remains `2`. The recorded
`None` or `SKIP` means no output update, not that the retained value was removed.
Before the first accepted reading, the output has no current value.

An optional return controls publication. An [optional parameter](optional-inputs.md)
controls whether evaluation may proceed without an input value. These are
separate decisions. To remove an existing current value, use
[invalidation](validity.md).
