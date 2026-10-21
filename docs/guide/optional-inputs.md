# Accept a missing input

To calculate before an offset is available, make the offset optional and choose
a fallback in node code. Required inputs still need current values.

Prerequisites: [Output publication](skip-output.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, test_graph

@node
def correct(reading: int, offset: int | None) -> int:
    return reading + (2 if offset is None else offset)

@graph
def temperature(reading: int, offset: int | None) -> int:
    return correct(reading, offset)

print(test_graph(temperature, [18, None, 19, 20], [None, 0, None, 3]))
```

Output:

```text
[20, 18, 19, 23]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <iostream>
#include <optional>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

const auto correct = node([](i64 reading, std::optional<i64> offset) {
    return reading + offset.value_or(2);
});

auto temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return correct(reading, offset);
}

int main() {
    const auto result = test_graph(graph(temperature), {18, SKIP, 19, 20}, {SKIP, 0, SKIP, 3});
    std::cout << std::boolalpha << result.equals({20, 18, 19, 23}) << '\n';
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

The first reading uses the fallback `2`. At step 2, an offset update alone
recalculates the current reading with offset `0`. At step 3, the offset remains
`0`; a skipped position does not make it absent.

Test absence explicitly. A zero offset is available and must not select the
fallback. Optionality permits evaluation with an invalid input; it does not
create a value or schedule evaluations without an update.

A plain optional parameter can also accept an absent scalar argument during
graph construction. That absence remains fixed for the run. A changing optional
input can become available or [invalid](validity.md) during execution.

For a library fallback, use [`default_`](../reference/library-selection.md#default_).
[`opt`](../reference/graph-code.md#optional-arguments) selects an operator's
optional overload; that overload defines its fallback behavior.
