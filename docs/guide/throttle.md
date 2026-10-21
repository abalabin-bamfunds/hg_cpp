# Limit publication frequency

To limit how often a changing result publishes, use `throttle`. It forwards the
first event immediately and combines later events during a period into the
latest pending state.

Prerequisites: [Evaluation time](time.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from bg import graph, test_graph
from bg.operators import throttle

@graph
def reported(reading: int) -> int:
    return throttle(reading, timedelta(microseconds=2))

print(test_graph(reported, [18, 19, 20, 21]))
```

Output:

```text
[18, None, 20, None, 21]
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

auto reported(wp<TS<i64>> reading) {
    return throttle(reading, timedelta_t{2});
}

int main() {
    std::cout << std::boolalpha << (test_graph(reported, {18, 19, 20, 21}).equals({18, SKIP, 20, SKIP, 21})) << '\n';
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

The update at time 2 contributes to the publication at that time. The pending
`19` is replaced by `20`; it is not replayed. The `21` publishes at time 4.
With no pending event, throttle stops scheduling until another input change.

`delay_first_tick=True` delays the first publication by a period too. The
period must be positive. Invalidation follows the same timing. Collection
publication reconciles the latest complete state, including removals.

Use [delay](delay.md) when every event must be retained. Use
[deduplication](dedup.md) when equality, rather than time, decides whether an
update is needed.
