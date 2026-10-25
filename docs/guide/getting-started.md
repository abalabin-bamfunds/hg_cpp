# Basic syntax

These examples cover the syntax used in small graphs. Each example is complete;
the linked chapters specify the behavior and additional forms.

## A fixed result

A graph function connects calculations. `const_` supplies a fixed graph value;
`run_graph` constructs and runs the graph and captures its output.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import const_


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
#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::operators;

auto example() {
    return const_(18);
}

int main() {
    const auto result = run_graph(example);
    std::cout << std::boolalpha << result.equals({18}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Python `@graph` declares the graph function and `-> int` declares its result type.
C++ uses an ordinary function with an inferred return type. `equals` checks the
captured C++ result against a literal sequence; `true` confirms the match.

## Expressions

Arithmetic on graph values connects calculations. Native arithmetic on language
literals, such as `18 + 2`, runs while the graph function executes.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import const_


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
#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::operators;

auto display_temperature() {
    const auto reading = const_(18);
    const auto calibrated = reading + 2;
    return calibrated * 10;
}

int main() {
    const auto result = run_graph(display_temperature);
    std::cout << std::boolalpha << result.equals({200}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

See [expressions and composition](graphs/composition.md#expressions).

## Graph functions

Parameters let a graph function reuse a calculation. Calling it from another graph
function adds its calculations to the same graph.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import const_


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
#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::operators;

auto calibrated_temperature(int reading, int offset) {
    return const_(reading) + offset;
}

auto display_temperature() {
    const auto calibrated = calibrated_temperature(18, 2);
    return calibrated * 10;
}

int main() {
    const auto result = run_graph(display_temperature);
    std::cout << std::boolalpha << result.equals({200}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

See [graph functions](graphs/composition.md#graph-functions).

## Changing inputs

A sequence passed to `run_graph` supplies successive input updates. `None` in Python
and `SKIP` in C++ mean that the input has no update at that position.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph


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
#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;

auto calibrated_temperature(wp<TS<i64>> reading, int offset) {
    return reading + offset;
}

auto display_temperature(wp<TS<i64>> reading) {
    const auto calibrated = calibrated_temperature(reading, 2);
    return calibrated * 10;
}

int main() {
using namespace literal;

const auto result = run_graph(display_temperature, {18, SKIP, 18, 19});
std::cout << std::boolalpha << result.equals({200, SKIP, 200, 210}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The repeated reading still produces an update. An empty output position means no
publication; the last result remains available. In C++, `wp<TS<i64>>` declares an
input connection carrying 64-bit integers. See [input sequences](graphs/updates.md).

## Conditionals

A changing condition needs a graph operation. `if_then_else` selects one input's
current value; both input calculations remain part of the graph.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import const_, if_then_else


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
#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::operators;

auto capped_temperature(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    const auto too_hot = calibrated > 21;
    return if_then_else(too_hot, const_(21), calibrated);
}

int main() {
    const auto result = run_graph(capped_temperature, {18, 20, 19});
    std::cout << std::boolalpha << result.equals({20, 21, 21}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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

Node code receives current values. Native `if`, loops, and helper functions work
on those values during evaluation.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph

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
#include <bg/library.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

const auto correct = node([](i64 reading) {
    if (reading < 0) {
        return i64{0};
    }
    return reading + 2;
});

auto temperature(wp<TS<i64>> reading) {
    return correct(reading);
}

int main() {
    const auto result = run_graph(graph(temperature), {18, -3, SKIP, 19});
    std::cout << std::boolalpha << result.equals({20, 0, SKIP, 21}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Python `@node` and C++ `node(callable)` declare the calculation. The surrounding
graph function connects its input. See [writing nodes](nodes/calculations.md).
