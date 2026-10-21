# Restart a running calculation

To calculate a total for each reporting session, pass a reset input to `sums`.
A valid reset update clears history, regardless of whether it carries true or
false. A reading arriving in the same evaluation starts the new history.

Prerequisites: [Running calculations](running-calculations.md), [validity](validity.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import sums

@graph
def session_total(reading: int, reset: bool) -> int:
    return sums(reading, reset=reset)

print(test_graph(session_total, [18, 19, None, 20, 21], [None, None, False, None, True]))
```

Output:

```text
[18, 37, None, 20, 21]
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

auto session_total(wp<TS<i64>> reading, wp<TS<bool>> reset) {
    return sums(reading, "reset"_n = reset);
}

int main() {
    std::cout << std::boolalpha << (test_graph(session_total, {18, 19, SKIP, 20, 21}, {SKIP, SKIP, false, SKIP, true}).equals({18, 37, INVALID, 20, 21})) << '\n';
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

The reset at step 3 removes the previous total. It does not sample the retained
reading `19`. The reading at step 4 starts a fresh total; the simultaneous reset
and reading at step 5 start another total with `21`.

`means`, `mins`, and `maxs` accept the same reset argument. Use `mins` and `maxs`
for extrema across successive readings; they publish the first value and then
only new extrema. Input invalidation also clears their history. A later valid
reading starts again. Invalidating the reset input does not reset history.

Compare this with [custom state](state.md), whose invalidation behavior must be
defined by the node author. See [stateful library reference](../reference/library-stateful.md)
for output types and guarantees.
