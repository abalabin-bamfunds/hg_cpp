# Aggregation and history

An aggregate combines current items. A running calculation combines successive
updates over time. The operations answer different questions.

| Requirement | Operations |
| --- | --- |
| Total, extrema, or mean of current items | `sum_`, `min_`, `max_`, `mean` |
| All or any current Boolean items | `all_`, `any_` |
| Application-specific combination of items | `reduce` in C++; collection node in Python |
| Count or previous update | `count`, `lag` |
| Total or mean across updates | `sums`, `means` |
| Difference between successive values | `diff` |
| Smoothed changing value | `ewma` |

## Current-item reductions

A changed item replaces its contribution. A removed item stops contributing.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, run_graph
from bg.operators import sum_

@graph
def total(readings: TSD[int, TS[int]]) -> int:
    return sum_(readings)

print(run_graph(total, [{1: 18, 2: 20}, {1: 19}, {2: REMOVE}]))
```

Output:

```text
[38, 39, 19]
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
#include <bg/time_series/tsd/tsd_type.hpp>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto total(wp<TSD<i64, TS<i64>>> readings) {
    return sum_(readings);
}

int main() {
    const auto result = run_graph(total, {{{1, 18}, {2, 20}}, {{1, 19}}, {{2, REMOVE}}});
    std::cout << result << '\n';
}
```

Output:

```text
[38, 39, 19]
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

The keyed input uses independent item updates, as in [iteration](iteration.md).
A valid empty collection has sum zero; extrema and mean have no result. Ordinary
reductions require valid immediate items. Optional reductions can ignore unavailable
items; see [collection contracts](../advanced-graphs/collections.md#aggregation-and-validity).

## Boolean reductions

`all_` requires every current item to be true. `any_` requires at least one true item.
For an empty collection, `all_` is true and `any_` is false.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, run_graph
from bg.operators import all_

@graph
def enabled(sensors: TSD[int, TS[bool]]) -> bool:
    return all_(sensors)

print(run_graph(enabled, [{1: True, 2: False}, {2: True}, {1: REMOVE, 2: REMOVE}]))
```

Output:

```text
[False, True, True]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto enabled(wp<TSD<i64, TS<bool>>> sensors) {
    return all_(sensors);
}

int main() {
    const auto result = run_graph(enabled,
        {{{1, true}, {2, false}}, {{2, true}}, {{1, REMOVE}, {2, REMOVE}}});
    std::cout << result << '\n';
}
```

Output:

```text
[false, true, true]
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

## Custom aggregation

An application-specific operation can combine current values. C++ `reduce` maintains
a reducer graph. Python uses a collection node for the equivalent product.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, compute_node, graph, run_graph

@compute_node
def product(values: TSD[int, TS[int]]) -> TS[int]:
    result = 1
    for item in values.valid_values():
        result *= item.value
    return result

@graph
def combined(values: TSD[int, TS[int]]) -> int:
    return product(values)

print(run_graph(combined, [{1: 2, 2: 3}, {1: 4}, {2: REMOVE}]))
```

Output:

```text
[6, 12, 4]
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
#include <bg/reduce.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::operators;
using namespace literal;

auto multiply(wp<TS<i64>> left, wp<TS<i64>> right) {
    return (left * right).as<TS<i64>>();
}

auto combined(wp<TSD<i64, TS<i64>>> values) {
    return reduce(multiply, values, "default"_n = i64{1});
}

int main() {
    const auto result = run_graph(combined, {{{1, 2}, {2, 3}}, {{1, 4}}, {{2, REMOVE}}});
    std::cout << result << '\n';
}
```

Output:

```text
[6, 12, 4]
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

The reducer must be associative and commutative; item order and tree shape are
unspecified. The product uses `1` when no valid items contribute. Python's collection
view is explained in [advanced nodes](../advanced-nodes/collections.md).

## Counts and previous values

`count` counts valid input updates. Repeated equal values are separate updates.
`lag` publishes the preceding input event when another event arrives.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import count


@graph
def reading_count(reading: int) -> int:
    return count(reading)


print(run_graph(reading_count, [None, 18, 18, None, 24, 22]))
```

Output:

```text
[None, 1, 2, None, 3, 4]
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
using namespace literal;

auto reading_count(wp<TS<i64>> reading) {
    return count(reading);
}

int main() {
    const auto result = run_graph(reading_count, {SKIP, 18, 18, SKIP, 24, 22});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 1, 2, None, 3, 4]
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

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import lag


@graph
def previous_temperature(reading: int) -> int:
    calibrated = reading + 2
    return lag(calibrated)


print(run_graph(previous_temperature, [None, 18, 18, None, 24, 22]))
```

Output:

```text
[None, None, 20, None, 20, 26]
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
using namespace literal;

auto previous_temperature(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return lag(calibrated);
}

int main() {
    const auto result = run_graph(previous_temperature, {SKIP, 18, 18, SKIP, 24, 22});
    std::cout << result << '\n';
}
```

Output:

```text
[None, None, 20, None, 20, 26]
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

A skipped position advances neither operation. `lag` retains one value; it does not
publish that final retained value without another input event.

## Running totals and means

`sums` and `means` accumulate valid input updates over time.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import sums


@graph
def temperature_total(reading: int) -> int:
    calibrated = reading + 2
    return sums(calibrated)


print(run_graph(temperature_total, [None, 18, 18, None, 24, 22]))
```

Output:

```text
[None, 20, 40, None, 66, 90]
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
using namespace literal;

auto temperature_total(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return sums(calibrated);
}

int main() {
    const auto result = run_graph(temperature_total, {SKIP, 18, 18, SKIP, 24, 22});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 20, 40, None, 66, 90]
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

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import means


@graph
def average_temperature(reading: int) -> float:
    calibrated = reading + 2
    return means(calibrated)


print(run_graph(average_temperature, [None, 18, 18, None, 24, 22]))
```

Output:

```text
[None, 20.0, 20.0, None, 22.0, 22.5]
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
using namespace literal;

auto average_temperature(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return means(calibrated);
}

int main() {
    const auto result = run_graph(average_temperature, {SKIP, 18, 18, SKIP, 24, 22});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 20, 20, None, 22, 22.5]
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

A repeated reading contributes again. Apply `dedup` first when only distinct
successive values should contribute.

## Reset signals

A reset clears the running history. If a value and reset update together, the
calculation starts the new history with that value.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import sums

@graph
def session_total(reading: int, reset: bool) -> int:
    return sums(reading, reset=reset)

print(run_graph(session_total, [18, 19, None, 20, 21], [None, None, False, None, True]))
```

Output:

```text
[18, 37, None, 20, 21]
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
using namespace bg::literals;
using namespace bg::operators;
using namespace literal;

auto session_total(wp<TS<i64>> reading, wp<TS<bool>> reset) {
    return sums(reading, "reset"_n = reset);
}

int main() {
    const auto result = run_graph(session_total, {18, 19, SKIP, 20, 21}, {SKIP, SKIP, false, SKIP, true});
    std::cout << result << '\n';
}
```

Output:

```text
[18, 37, None, 20, 21]
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

Each operation defines its invalidation and reset rules in the
[stateful library reference](../../reference/library-stateful.md).

`diff` starts from the second valid value. `ewma` weights new values against its
retained average. The [analytical examples](../../reference/library-stateful.md#diff)
show both calculations. Invalidation clears their retained history.
