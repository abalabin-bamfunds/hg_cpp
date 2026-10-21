# Keep state in a node

To retain information specific to one calculation instance, request `State`.
Use library [running calculations](running-calculations.md) for standard totals
and means; custom state supports application-specific rules.

Prerequisites: [Calculation nodes](calculation-nodes.md), [running calculations](running-calculations.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import State, graph, node, test_graph

class Readings:
    def __init__(self):
        self.peak = 0

@node
def peak_above_zero(reading: int, state: State[Readings] = None) -> int:
    state.peak = max(state.peak, reading)
    return state.peak

@graph
def peak(reading: int) -> int:
    return peak_above_zero(reading)

print(test_graph(peak, [18, 17, 21, None, 19]))
```

Output:

```text
[18, 18, 21, None, 21]
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
#include <algorithm>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

struct Readings {
    i64 peak{};
};

struct PeakAboveZero {
    In<TS<i64>> reading;
    State<Readings> state;
    Out<TS<i64>> output;

    static constexpr bool deduplicate_wiring = false;

    void evaluate() const {
        state->peak = std::max(state->peak, *reading);
        output.set(state->peak);
    }
};

const auto peak_above_zero = node<PeakAboveZero>();

auto peak(wp<TS<i64>> reading) {
    return peak_above_zero(reading);
}

int main() {
    const auto result = test_graph(graph(peak), {18, 17, 21, SKIP, 19});
    std::cout << std::boolalpha << result.equals({18, 18, 21, SKIP, 21}) << '\n';
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

Python `State[Readings]` calls `Readings()` once for each runtime node and passes
the resulting object to callbacks. Its `= None` default marks an injected
parameter: graph code does not supply it. C++ `State<Readings>` gives access to
that node's retained `Readings` value. The aggregate declares input, state, and
output fields; its `const evaluate()` mutates state and publishes through the
field views.

A new graph run starts with new state. Python stateful nodes have separate
state on each call. C++ normally reuses equal node calls;
`deduplicate_wiring = false` requests separate runtime nodes when independent
state is required, as in this example. See [state reference](../reference/state.md#state).

A skipped step does not evaluate the node. This callback publishes on each valid reading, including when the
peak remains equal.

Required-input invalidation invalidates the output; it does not automatically
reset arbitrary custom state. Define reset behavior explicitly when the
application requires it. Library running calculations document their own reset
and invalidation rules.

Use [lifecycle callbacks](lifecycle.md) for resources that must be acquired or
released with a graph instance. See [state and lifecycle reference](../reference/state.md).
