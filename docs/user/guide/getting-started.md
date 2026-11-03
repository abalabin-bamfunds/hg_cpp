# Basic syntax

Graph code uses familiar Python or C++ expressions to describe calculations that HG
keeps up to date as their inputs change. The [opening example](../index.md) compares
this with an ordinary function call. The examples here cover the basic syntax, with
links to the chapters that explain each feature in more detail.

## A fixed result

If a calculation needs a known value, such as a fixed calibration or configuration
setting, use `const_` to supply it. `run_graph` constructs the graph, evaluates its
calculations, and captures the output so you can inspect the result.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import const_


@graph
def example() -> int:
    return const_(18)


print(run_graph(example))
```

Output:

```text
[18]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;

auto example() {
    return const_(18);
}

int main() {
    const auto result = run_graph(example);
    std::cout << result << '\n';
}
```

Output:

```text
[18]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

In Python, `@graph` declares the graph function and `-> int` gives its result type;
C++ uses an ordinary function and infers the return type. Both examples print the
captured updates as a list, using Python `print` or C++ `std::cout`.

## Expressions

To derive a result from a graph input, use arithmetic expressions much as you would
with ordinary values. HG records the calculation and its dependencies so the result
can update with its inputs. An expression using only native literals, such as `18 + 2`,
still computes immediately while the graph is being constructed.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import const_


@graph
def display_temperature() -> int:
    reading = const_(18)
    calibrated = reading + 2
    return calibrated * 10


print(run_graph(display_temperature))
```

Output:

```text
[200]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;

auto display_temperature() {
    const auto reading = const_(18);
    const auto calibrated = reading + 2;
    return calibrated * 10;
}

int main() {
    const auto result = run_graph(display_temperature);
    std::cout << result << '\n';
}
```

Output:

```text
[200]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

See [expressions and composition](graphs/composition.md#expressions).

## Graph functions

If several parts of an application need the same calculation, put it in a graph
function. Each call connects that calculation to the supplied inputs and fixed
configuration, just as an ordinary function call reuses value code.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import const_


@graph
def calibrated_temperature(reading: int, offset: int) -> int:
    return const_(reading) + offset


@graph
def display_temperature() -> int:
    calibrated = calibrated_temperature(18, 2)
    return calibrated * 10


print(run_graph(display_temperature))
```

Output:

```text
[200]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;

auto calibrated_temperature(int reading, int offset) {
    return const_(reading) + offset;
}

auto display_temperature() {
    const auto calibrated = calibrated_temperature(18, 2);
    return calibrated * 10;
}

int main() {
    const auto result = run_graph(display_temperature);
    std::cout << result << '\n';
}
```

Output:

```text
[200]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

See [graph functions](graphs/composition.md#graph-functions).

## Changing inputs

To see how a calculation responds to new readings, pass a sequence of input updates
to `run_graph`. The graph keeps its connections and updates the result as each reading
arrives; `None` in Python and `SKIP` in C++ leave the input unchanged at that position.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph


@graph
def calibrated_temperature(reading: int, offset: int) -> int:
    return reading + offset


@graph
def display_temperature(reading: int) -> int:
    calibrated = calibrated_temperature(reading, 2)
    return calibrated * 10


print(run_graph(display_temperature, [18, None, 18, 19]))
```

Output:

```text
[200, None, 200, 210]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;

auto calibrated_temperature(auto reading, int offset) {
    return reading + offset;
}

int main() {
    using namespace literal;

    const auto result = run_graph(
        [](auto reading) {
            const auto calibrated = calibrated_temperature(reading, 2);
            return calibrated * 10;
        },
        {18, SKIP, 18, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[200, None, 200, 210]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The repeated reading still produces an update. An empty output position means no
publication; the last result remains available. C++ `auto` parameters receive input
connections, and `run_graph` infers their value types from the supplied sequences.
The lambda connects those inputs to the calculation. See
[input sequences](graphs/updates.md).

## Conditionals

If a result needs to choose between values as a condition changes, use
`if_then_else`. The example caps a reading at a fixed limit, choosing again whenever
the reading changes. Both input calculations remain connected; native `if` chooses
connections only while the graph is being constructed.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import const_, if_then_else


@graph
def capped_temperature(reading: int) -> int:
    calibrated = reading + 2
    too_hot = calibrated > 21
    return if_then_else(too_hot, const_(21), calibrated)


print(run_graph(capped_temperature, [18, 20, 19]))
```

Output:

```text
[20, 21, 21]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;

int main() {
    const auto result = run_graph(
        [](auto reading) {
            const auto calibrated = reading + 2;
            const auto too_hot = calibrated > 21;
            return if_then_else(too_hot, const_(21), calibrated);
        },
        {18, 20, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 21, 21]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

[Conditionals](graphs/conditionals.md) also covers native `if`, conditional
forwarding, fallbacks, and selecting graph instances with `switch_`.

## Calculation nodes

When a calculation needs application logic that the library does not provide, write
a node. Its callback receives current values, so you can use ordinary `if` statements,
loops, and helper functions while the graph evaluates.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph

@node
def correct(reading: int) -> int:
    if reading < 0:
        return 0
    return reading + 2

@graph
def temperature(reading: int) -> int:
    return correct(reading)

print(run_graph(temperature, [18, -3, None, 19]))
```

Output:

```text
[20, 0, None, 21]
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

using namespace hg;
using namespace hg::operators;
using namespace literal;

const auto correct = node([](i64 reading) {
    if (reading < 0) {
        return i64{0};
    }
    return reading + 2;
});

int main() {
    const auto result = run_graph(
        [](auto reading) {
            return correct(reading);
        },
        {18, -3, SKIP, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 0, None, 21]
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Python `@node` and C++ `node(callable)` declare the calculation. The surrounding
graph function connects its input. See [writing nodes](nodes/calculations.md).
