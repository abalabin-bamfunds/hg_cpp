# Time series types

Plain value annotations are sufficient for ordinary calculations. Explicit time
series types are required when a connection must express collection shape,
publication constraints, or reference behavior.

## Values and time series

A time series retains its current value and last updated time. It does not retain
past values. `TS[T]` in Python and `TS<T>` in C++ describe a time series value of type `T`.

| Requirement | Declaration |
| --- | --- |
| Ordinary integer values | Plain `int` / `i64`, or explicit `TS[int]` / `TS<i64>` |
| One complete dictionary value | `TS[dict[K, V]]` / `TS<Dict<K, V>>` |
| Independently changing dictionary items | `TSD[K, TS[V]]` / `TSD<K, TS<V>>` |
| A selected source | `REF[TS[T]]` / `REF<TS<T>>` |

## Validity

A time series is valid when it has a current value. Invalidation removes that value.
Skipping an update preserves it. `valid` exposes availability as a Boolean result.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import INVALID, graph, run_graph
from bg.operators import valid

@graph
def has_temperature(reading: int) -> bool:
    return valid(reading + 2)

print(run_graph(has_temperature, [None, 18, None, INVALID, 19]))
```

Output:

```text
[False, True, None, False, True]
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

auto has_temperature(wp<TS<i64>> reading) {
    return valid(reading + 2);
}

int main() {
    const auto result = run_graph(has_temperature, {SKIP, 18, SKIP, INVALID, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[false, true, None, false, true]
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

| Event | Current value after the event | Publication |
| --- | --- | --- |
| No update before the first value | Unavailable | None |
| Value update | Supplied value | Value |
| No update after a value | Retained value | None |
| Invalidation | Unavailable | Invalidation |
| Value after invalidation | New value | Value |

Required calculations propagate invalidation and resume when their inputs become
valid. Python dense result positions use `None` for both no update and invalidation;
observe `valid` when the distinction matters. C++ literal comparisons distinguish
`SKIP` and `INVALID`.

## Matching time series types

The receiving input must accept the output's value type and time series shape.
A scalar dictionary and a dictionary of time series require an explicit conversion
when their shapes differ.

The same value type can carry different [guarantees](guarantees.md). A reference can
bind to an ordinary value input through automatic dereferencing. The
[type reference](../../reference/types.md) defines complete compatibility rules.

## Converting collection shapes

`convert` copies the current value into a requested shape. This example gives a
scalar sequence's two positions independent time series outputs.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, TSL, graph, run_graph
from bg.operators import convert

@graph
def first(prices: tuple[int, ...]) -> int:
    items = convert[TSL[TS[int], 2]](prices)
    return items[0]

print(run_graph(first, [(10, 20), (12, 20), (12, 25)]))
```

Output:

```text
[10, 12, 12]
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

auto first(wp<TS<List<i64>>> prices) {
    auto items = convert[type<TSL<TS<i64>, 2>>](prices).as<TSL<TS<i64>, 2>>();
    return items[i64{0}];
}

int main() {
    const auto result = run_graph(first, {{10, 20}, {12, 20}, {12, 25}});
    std::cout << result << '\n';
}
```

Output:

```text
[10, 12, 12]
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

The source still publishes complete scalar lists. Conversion publishes their items;
it does not deduplicate equal values. A list with the wrong length fails during
evaluation. [Collection conversion reference](../../reference/library-collections.md)
lists supported shapes and validity requirements.
