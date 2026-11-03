# Shared dependencies and services

When several calculations need the same prices, configuration, or external service,
you can share a connection through arguments or context, or request an implementation
through a service. Shared scalar state covers application data that an outer graph
and its inner graphs need to access together.

## Context time series

If passing the same dependency through several C++ graph functions obscures their
main calculation, a context input can obtain it by name from an enclosing graph
scope. Explicit arguments keep the dependency visible at the call and are currently
the equivalent form available in Python.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph

@graph
def calibrated(reading: int, offset: int) -> int:
    return reading + offset

@graph
def temperature(reading: int, offset: int) -> int:
    return calibrated(reading, offset)

print(run_graph(temperature, [18, 19], [2, 3]))
```

Output:

```text
[20, 22]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/graph.hpp>

using namespace hg;
using namespace hg::literals;
using namespace hg::operators;
using namespace literal;

const auto calibrated = node([](i64 reading, In<TS<i64>, "offset"_n, context> offset) {
    return reading + *offset;
});

auto temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return with_context("offset"_n = offset, [&] { return calibrated(reading); });
}

int main() {
    const auto result = run_graph(temperature, {18, 19}, {2, 3});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 22]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

A context requirement resolves by name and compatible time series type during
construction. An explicit named argument can supply that requirement.
[Context reference](../../reference/graph-code.md#context-inputs) defines lookup.

## Shared scalar state

If outer and inner C++ graphs need access to the same application data, use
`GlobalState` to share scalar storage. Python applications can retain
application-owned storage in sink callbacks for application effects.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph
from hgraph.operators import const_

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/global_state.hpp>
#include <hg/evaluation_context.hpp>

using namespace hg;
using namespace hg::operators;
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

auto retained_result() {
    auto state = GlobalState{};
    run_graph([] { retain(const_(18) + 2); }, GraphRunOptions{.global_state = &state});
    auto result = i64{0};
    state.read([&](const auto& values) { result = *scalar_cast<i64>(values.at("temperature")); });
    return result;
}

int main() {
    std::cout << retained_result() << '\n';
}
```

Output:

```text
20
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

This storage is not a time series. Changing it does not publish graph updates or
create dependencies. [State reference](../../reference/state.md#shared-application-state) specifies
ownership and access.

## Services

To write a graph against a data or request interface while choosing its implementation
for each run, use a service. The graph requests the service, and a catalogue supplies
the matching implementation.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, ServiceCatalogue, graph, reference_service, run_graph
from hgraph.operators import const_

@reference_service
def offset() -> TS[int]:
    pass

@graph
def offset_implementation() -> TS[int]:
    return const_(2)

catalogue = ServiceCatalogue()
catalogue.register_impl(offset, offset_implementation)

@graph
def temperature(reading: int) -> int:
    return reading + offset()

print(run_graph(temperature, [18, 19], catalogue=catalogue))
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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/graph.hpp>
#include <hg/service.hpp>
#include <hg/service_catalogue.hpp>

using namespace hg;
using namespace hg::operators;
using namespace literal;

const auto offset = reference_service<auto()->TS<i64>>{"offset"};
const auto offset_implementation = graph([] { return const_(i64{2}).as<TS<i64>>(); });

auto temperature(wp<TS<i64>> reading) { return reading + offset(); }

auto run_service() {
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(offset, offset_implementation);
    return run_graph(temperature, GraphRunOptions{.services = &catalogue}, {18, 19});
}

int main() {
    std::cout << run_service() << '\n';
}
```

Output:

```text
[20, 21]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The service reference defines compatible signatures, registration, and instance
ownership. Subscription services can maintain per-key state; their key and lifetime
contracts are specified in [services](../../reference/services.md).
