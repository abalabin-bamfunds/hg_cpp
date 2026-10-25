# Expressions and composition

Graph functions connect existing calculations into larger calculations. They run
during construction; BG evaluates the connected nodes when the graph runs.

## Expressions

An expression containing a graph value creates a dependency. Named intermediate
results can be returned, passed to another calculation, or used by several consumers.

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

`calibrated` identifies the addition's output. The multiplication reads that output.
The result follows dependency order regardless of the order in which consumers use it.

The library supports arithmetic, comparisons, Boolean operations, text operations,
and conversions. [Numeric reference](../../reference/library-numeric.md) specifies
supported types and numeric behavior.

## Graph functions

A graph function groups connections behind parameters and a result. A scalar
argument supplies a fixed value; a connection preserves its dependency.

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

The inner call constructs the calibration calculation. It does not start a separate
run. Reusing an intermediate output connects additional consumers to the same value.

## Fixed parameters

Native control flow can use a fixed scalar argument while constructing a graph.
Here, `apply_offset` selects the calculation for the complete run.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph


@graph
def calibrated_temperature(reading: int, apply_offset: bool) -> int:
    offset = 0
    if apply_offset:
        offset = 2
    return reading + offset


@graph
def reported_temperature(reading: int) -> int:
    return calibrated_temperature(reading, True)


print(run_graph(reported_temperature, [18, 20, 19]))
```

Output:

```text
[20, 22, 21]
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

auto calibrated_temperature(wp<TS<i64>> reading, bool apply_offset) {
    auto offset = 0;
    if (apply_offset) {
        offset = 2;
    }
    return reading + offset;
}

auto reported_temperature(wp<TS<i64>> reading) {
    return calibrated_temperature(reading, true);
}

int main() {
    const auto result = run_graph(reported_temperature, {18, 20, 19});
    std::cout << std::boolalpha << result.equals({20, 22, 21}) << '\n';
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

`apply_offset` receives `True` or `true` from the outer graph. A changing Boolean
input instead requires a [conditional graph operation](conditionals.md).
