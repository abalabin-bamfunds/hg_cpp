# Shared dependencies and services

A shared dependency can be passed explicitly, supplied by context, or implemented as
a service. Shared scalar state stores application data rather than graph connections.

## Context time series

An explicit argument makes a dependency visible at the call. C++ context inputs allow
a named dependency to be supplied by an enclosing graph scope. Python currently uses
explicit arguments for the equivalent calculation.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/graph.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::operators;
using namespace literal;

const auto calibrated = node([](i64 reading, In<TS<i64>, "offset"_n, context> offset) {
    return reading + *offset;
});

auto temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return with_context("offset"_n = offset, [&] { return calibrated(reading); });
}

int main() {
    const auto result = run_graph(temperature, {18, 19}, {2, 3});
    std::cout << std::boolalpha << result.equals({20, 22}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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

C++ `GlobalState` makes scalar application data available across outer and inner
graphs. Python applications can retain application-owned storage in sink callbacks.

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
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/global_state.hpp>
#include <bg/evaluation_context.hpp>

using namespace bg;
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

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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

A service separates a graph's dependency from its implementation. A catalogue supplies
the implementation selected for the run.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, ServiceCatalogue, graph, reference_service, run_graph
from bg.operators import const_

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/graph.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>

using namespace bg;
using namespace bg::operators;
using namespace literal;

const auto offset = reference_service<auto()->TS<i64>>{"offset"};
const auto offset_implementation = graph([] { return const_(i64{2}).as<TS<i64>>(); });

auto temperature(wp<TS<i64>> reading) { return reading + offset(); }

bool check_service() {
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(offset, offset_implementation);
    return run_graph(temperature, GraphRunOptions{.services = &catalogue}, {18, 19}).equals({20, 21});
}

int main() {
    std::cout << std::boolalpha << (check_service()) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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
