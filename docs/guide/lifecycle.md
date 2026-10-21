# Initialize and release resources

To acquire a resource once per graph instance and release it when that instance
stops, define start and stop callbacks. Retain the resource in node state.

Prerequisites: [Node state](state.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import State, graph, node, test_graph

class Session:
    def __init__(self):
        self.offset = 0

@node
def corrected(reading: int, state: State[Session] = None) -> int:
    return reading + state.offset

@corrected.start
def start(state: State[Session]):
    state.offset = 2

@corrected.stop
def stop(state: State[Session]):
    state.offset = 0

@graph
def temperature(reading: int) -> int:
    return corrected(reading)

print(test_graph(temperature, [18, 19]))
```

Output:

```text
[20, 21]
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

struct Session {
    i64 offset{};
};

struct Corrected {
    In<TS<i64>> reading;
    State<Session> session;
    Out<TS<i64>> output;

    void start() const { session->offset = 2; }
    void evaluate() const { output.set(*reading + session->offset); }
    void stop() const noexcept { session->offset = 0; }
};

const auto corrected = node<Corrected>();

auto temperature(wp<TS<i64>> reading) {
    return corrected(reading);
}

int main() {
    const auto result = test_graph(temperature, {18, 19});
    std::cout << std::boolalpha << result.equals({20, 21}) << '\n';
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

The example initializes an offset instead of opening an external resource.
A resource-owning node can use the same lifecycle for a connection or file.
Start runs before evaluation. Stop runs during graph cleanup, including when
execution fails. Cleanup must release resources without throwing.

Python callbacks select parameters from the node declaration by name. Their
order can differ, but an undeclared name is an error. A start callback's return
value does not publish output. C++ aggregate nodes expose inputs, state, and
output as fields and define `const` callback methods.

A [map branch](map.md) owns a separate resource for each active key. A
[switch case](switch.md) starts when selected and stops when replaced. Returning
to a stopped case creates a fresh instance.

Python stop exceptions are reported through the unraisable-exception hook after
cleanup. C++ stop callbacks must not throw. See [lifecycle reference](../reference/state.md).
