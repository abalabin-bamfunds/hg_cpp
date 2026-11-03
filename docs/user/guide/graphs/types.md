# Types and connections

Type declarations let you state which values a calculation accepts and produces,
so incompatible connections are caught before the graph processes inputs. HG checks
each output against its receiving parameter while constructing the graph.

## Concrete value types

For calculations that only need current values, plain annotations describe the
required types. Python `int` maps to HG's signed 64-bit integer, written `i64` in
the C++ examples.

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
#include <hg/graph.hpp>
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

auto temperature(wp<TS<i64>> reading) {
    return correct(reading);
}

int main() {
    const auto result = run_graph(graph(temperature), {18, -3, SKIP, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 0, None, 21]
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

`correct` requires an integer input and produces an integer output. The graph's
result declaration must accept that output. A required input prevents calculation
until it has a current value.

## Operator matching and conversion

If a calculation combines different numeric types, use `convert` to make the intended
conversion explicit. Built-in numeric operations select an implementation for matching
types, so multiplying an integer graph value by `0.5` first requires a floating-point
input.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, graph, run_graph
from hgraph.operators import convert

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto scaled(wp<TS<i64>> reading) {
    return convert[type<TS<double>>](reading) * 0.5;
}

int main() {
    const auto result = run_graph(scaled, {18, SKIP, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[9, None, 9.5]
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

`convert[TS[float]]` in Python and `convert[type<TS<double>>]` in C++ request a
floating-point result. This is value conversion. C++ `.as<TS<T>>()` checks the
connection's type and does not convert its values.

## Type errors

If HG cannot connect a calculation, its diagnostic identifies the operation and the
supplied types. The connection must be corrected before any input sequence can run;
[errors and diagnostics](../../how-to/errors.md#type-errors) shows a mismatch and its
correction.

[Advanced type matching](../advanced-graphs/guarantees.md) covers generic types and
publication constraints. [Scalar reference](../../reference/scalars.md) specifies
all language mappings and supported schemas.
