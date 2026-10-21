# Delay every update

To preserve each input event and publish it later, use `delay(value, duration)`.
Delay retains the event's value, so later source changes do not replace it.

Prerequisites: [Evaluation time](time.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from bg import graph, test_graph
from bg.operators import delay

@graph
def delayed(reading: int) -> int:
    return delay(reading, timedelta(microseconds=2))

print(test_graph(delayed, [18, 19, 19]))
```

Output:

```text
[None, None, 18, 19, 19]
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

auto delayed(wp<TS<i64>> reading) {
    return delay(reading, timedelta_t{2});
}

int main() {
    const auto result = test_graph(delayed, {18, 19, 19});
    std::cout << std::boolalpha << result.equals({SKIP, SKIP, 18, 19, 19}) << '\n';
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

Each publication occurs at the input event's evaluation time plus two
microseconds. The repeated `19` remains a separate event. Simulation advances
directly to these times.

A zero duration returns the input. A negative duration is a wiring error.
Invalidation is delayed as an event too. Delayed work stops when its graph
stops, including when a selected inner graph is replaced.

For fewer publications that retain only the latest pending state, use
[throttle](throttle.md). [Timed operator reference](../reference/library-time.md)
lists the supported collection and reference forms.
