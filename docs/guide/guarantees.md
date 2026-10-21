# Declare publication constraints

To require an input that cannot repeat a value update, use an explicit guarantee.
For example, a final input supplies at most one value and can represent a
configuration resolved once. Use `dedup` when the need is to suppress equal
updates from an ordinary source.

Prerequisites: [Deduplication](dedup.md), [validity](validity.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, final, graph, test_graph

@graph
def configured(offset: TS[int, final]) -> int:
    return offset + 18

print(test_graph(configured, [2, None, None]))
```

Output:

```text
[20, None, None]
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

auto configured(wp<TS<i64, TimeSeriesGuarantee::final>> offset) {
    return offset + 18;
}

int main() {
    std::cout << std::boolalpha << (test_graph(configured, {2, SKIP, SKIP}).equals({20, SKIP, SKIP})) << '\n';
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

The annotation is a contract, not a filter. A second valid input publication
would violate it. A final input can remain unavailable throughout a run.
A const input publishes once at its start; an ordinary final input may resolve
later.

A deduplicated time series suppresses equal requested values. An ordinary time
series permits equal-valued updates. Stronger guarantees satisfy weaker input
requirements; omitting an explicit input guarantee accepts the source guarantee.

Collection guarantees also apply to their items. A reference has a separate
guarantee for retargeting, so observing its target can have weaker publication
constraints than an individual target. [Type reference](../reference/types.md#guarantees)
defines compatibility and propagation. Subscription services use final keys
because a subscription identifies one key for that client.
