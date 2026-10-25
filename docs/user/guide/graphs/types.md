# Types and connections

A connection is valid when the output type satisfies the receiving parameter.
BG checks connections while constructing the graph, before processing input values.

## Concrete value types

Plain annotations describe the values used by a calculation. Python `int` maps to
BG's signed 64-bit integer; examples use `i64` for the corresponding C++ value.

| Python | C++ | Value |
| --- | --- | --- |
| `int` | `i64` | Signed 64-bit integer |
| `float` | `double` | Floating-point number |
| `bool` | `bool` | Boolean |
| `str` | `string` | Text |
| `tuple[int, ...]` | `List<i64>` | Homogeneous scalar sequence |
| `dict[str, int]` | `Dict<string, i64>` | Scalar dictionary |

The graph function connects outputs; a node receives their current values:

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

`correct` requires an integer input and produces an integer output. The graph's
result declaration must accept that output. A required input prevents calculation
until it has a current value.

## Operator matching and conversion

Operators select an implementation from argument types. Built-in numeric operations
use matching numeric types. Multiplying an integer graph value by `0.5` requires
an explicit conversion to floating point.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, graph, run_graph
from bg.operators import convert

@graph
def scaled(reading: int) -> float:
    return convert[TS[float]](reading) * 0.5

print(run_graph(scaled, [18, None, 19]))
```

Output:

```text
[9.0, None, 9.5]
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

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto scaled(wp<TS<i64>> reading) {
    return convert[type<TS<double>>](reading) * 0.5;
}

int main() {
    const auto result = run_graph(scaled, {18, SKIP, 19});
    std::cout << std::boolalpha << result.equals({9.0, SKIP, 9.5}) << '\n';
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

`convert[TS[float]]` in Python and `convert[type<TS<double>>]` in C++ request a
floating-point result. This is value conversion. C++ `.as<TS<T>>()` checks the
connection's type and does not convert its values.

## Type errors

A mismatch identifies the operation and supplied types. A failed connection is a
graph-construction error; no input sequence can repair it. A concrete example and
the diagnostic appear in [errors and diagnostics](../../how-to/errors.md#type-errors).

[Advanced type matching](../advanced-graphs/guarantees.md) covers generic types and
publication constraints. [Scalar reference](../../reference/scalars.md) specifies
all language mappings and supported schemas.
