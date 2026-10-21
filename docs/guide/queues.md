# Receive external updates

To submit values from an external producer, use a queue source. A queue handle
can survive its callback and submit later messages while the graph is running.
The finite example writes one message at graph start.

Prerequisites: [Running graphs](run-graphs.md), [lifecycle callbacks](lifecycle.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import Queue, TS, graph, queue, test_graph

@queue
def readings(q: Queue = None) -> TS[int]:
    q.write(18)

@graph
def temperature() -> int:
    return readings() + 2

print(test_graph(temperature))
```

Output:

```text
[None, 20]
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
#include <bg/queue.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

const auto readings = queue<TS<i64>>(start = [](Queue q) {
    q.write(TimeSeriesLiteral{18});
});

auto temperature() { return readings() + 2; }

int main() {
    std::cout << std::boolalpha << (test_graph(temperature).equals({SKIP, 20})) << '\n';
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

The queued message is delivered after start. The queue callback's return does
not publish a value. `write` reports whether a message committed. A closed
handle returns false; conversion failures on an open handle propagate without
committing a partial message.

For later producer messages, use realtime execution so the graph waits while
idle. Give each producer an independent copy of the handle and release or stop
producers through lifecycle cleanup. Do not overlap writes or copying through
one handle during a write. Shutdown closes writers before stop callbacks.

Standard mode preserves messages. Elide mode combines pending messages into
the latest state. Batch mode groups leaf values into scalar lists. Choose the
mode from the application's delivery requirement; batching changes the output
leaf type. See [queue reference](../reference/sources.md#queues).
