# Produce periodic signals

To request work periodically, use `schedule`. A bounded source gives a finite
simulation and makes its termination explicit. Each signal carries `True`.

Prerequisites: [Evaluation time](time.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from bg import graph, test_graph
from bg.operators import count, schedule

@graph
def requests() -> int:
    return count(schedule(timedelta(microseconds=1), max_ticks=3))

print(test_graph(requests))
```

Output:

```text
[None, 1, 2, 3]
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
using namespace bg::literals;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto requests() {
    return count(schedule(timedelta_t{1}, "max_ticks"_n = 3));
}

int main() {
    const auto result = test_graph(requests);
    std::cout << std::boolalpha << result.equals({SKIP, 1, 2, 3}) << '\n';
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

C++ `"max_ticks"_n = 3` supplies a named graph argument. The `_n` suffix
comes from `bg::literals`. Python uses ordinary keyword arguments.

The default first signal occurs one period after graph start. `initial_delay=False`
in Python or `"initial_delay"_n = false` in C++ includes a signal at graph start.
`max_ticks=0` produces none; omitting the limit produces an ongoing source.
The period must be positive.

The initial empty position belongs to the test's start time. Test positions are
one microsecond apart; scheduled work can extend the recorded sequence beyond
any input sequence. See [test recording](../reference/testing.md).

For periodic reading of a changing value, connect this signal to
[`sample`](sample.md). Counting the signal counts requests, not elapsed seconds.
See [evaluation time](time.md) for time units and execution modes.
