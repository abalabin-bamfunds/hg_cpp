# Retain application results

To retain results after graph execution, connect a sink to application-owned
storage. Python can use a sink callback with an owned result container. C++
also provides `GlobalState` shared by the outer graph and its inner graphs.

Prerequisites: [Sources and sinks](run-graphs.md), [node state](state.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph
from bg.operators import const_

results = {}

@node
def retain(value: int) -> None:
    results["temperature"] = value

@graph
def application() -> None:
    retain(const_(18) + 2)

run_graph(application)
print(results["temperature"])
```

Output:

```text
20
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
#include <bg/global_state.hpp>
#include <bg/run_graph.hpp>
#include <bg/evaluation_context.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

struct Retain {
    In<TS<i64>> value;

    void evaluate(EvaluationContext* context) const {
        context->global_state().write([&](auto& values) {
            values.set(util::symbol{"temperature"}, ScalarValue<i64>{*value});
        });
    }
};

const auto retain = node<Retain>();

bool retained_result() {
    auto state = GlobalState{};
    run_graph([] { retain(const_(18) + 2); }, GraphRunOptions{.global_state = &state});
    auto result = i64{0};
    state.read([&](const auto& values) { result = *scalar_cast<i64>(values.at("temperature")); });
    return result == 20;
}

int main() {
    std::cout << std::boolalpha << (retained_result()) << '\n';
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

The Python dictionary belongs to the application; it is not a graph scalar
argument. The callback writes a sink effect. Provide synchronization when
application threads access such data concurrently.

C++ `GlobalState` owns stored scalar values. Read callbacks borrow entries;
write callbacks exclude other access. A supplied state object must outlive the
runtime graph. A runner-created state object lasts only for that run.

Changing stored data does not notify graph consumers. Use a time series input
for dependencies that must trigger calculations. Keep per-node history in
[`State`](state.md). Shared-state changes are not rolled back if a later node
fails. See [state reference](../reference/state.md#shared-application-state).
