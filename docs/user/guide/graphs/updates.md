# Changing inputs

`run_graph` can supply successive updates to graph inputs and capture output events.
Each input retains its current value between updates.

## Input sequences

Each sequence position represents an evaluation time, one microsecond after the
previous position. `None` in Python and `SKIP` in C++ supply no input update.

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
std::cout << result << '\n';
}
```

Output:

```text
[200, None, 200, 210]
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

| Position | Reading update | Current reading | Output update |
| --- | --- | --- | --- |
| 1 | 18 | 18 | 200 |
| 2 | No update | 18 | No update |
| 3 | 18 | 18 | 200 |
| 4 | 19 | 19 | 210 |

A skipped position preserves current values. An equal value still counts as an
update. Captured results show publications, rather than repeating the retained
output at every position.

## Multiple inputs

One sequence per input describes independent updates. BG evaluates a calculation
when its required inputs have values and an active dependency updates.

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
def display_temperature(reading: int, offset: int) -> int:
    calibrated = calibrated_temperature(reading, offset)
    return calibrated * 10


print(run_graph(
    display_temperature,
    [18, None, 19, None],
    [None, 2, None, 3],
))
```

Output:

```text
[None, 200, 210, 220]
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
using namespace literal;

auto calibrated_temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return reading + offset;
}

auto display_temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    const auto calibrated = calibrated_temperature(reading, offset);
    return calibrated * 10;
}

int main() {
    const auto result = run_graph(
        display_temperature,
        {18, SKIP, 19, SKIP},
        {SKIP, 2, SKIP, 3});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 200, 210, 220]
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

The first reading waits for the offset. The offset's update then calculates using
the current reading. Updates at the same position are applied before dependent
calculations evaluate; they do not produce intermediate partial results.

## Repeated values

`dedup` suppresses an update whose value equals the current value. Placement controls
which downstream calculations observe repeated input values.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import dedup


@graph
def reported_temperature(reading: int) -> int:
    calibrated = reading + 2
    return dedup(calibrated)


print(run_graph(reported_temperature, [18, 18, None, 19, 18, 18]))
```

Output:

```text
[20, None, None, 21, 20, None]
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

auto reported_temperature(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return dedup(calibrated);
}

int main() {
    const auto result = run_graph(reported_temperature, {18, 18, SKIP, 19, 18, 18});
    std::cout << result << '\n';
}
```

Output:

```text
[20, None, None, 21, 20, None]
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

Apply `dedup` before an expensive calculation to suppress its repeated inputs.
Apply it after a calculation when only equal results should be suppressed.

## Signal-driven publication

`sample(signal, value)` publishes the current value when the signal updates.
Changes to `value` alone do not publish a sampled result.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import sample


@graph
def sampled_temperature(reading: int, signal: bool) -> int:
    calibrated = reading + 2
    return sample(signal, calibrated)


print(run_graph(
    sampled_temperature,
    [18, 19, None, 20, 21, None],
    [None, True, True, None, True, False],
))
```

Output:

```text
[None, 21, 21, None, 23, 23]
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

auto sampled_temperature(wp<TS<i64>> reading, wp<TS<bool>> signal) {
    const auto calibrated = reading + 2;
    return sample(signal, calibrated);
}

int main() {
    const auto result = run_graph(
        sampled_temperature,
        {18, 19, SKIP, 20, 21, SKIP},
        {SKIP, true, true, SKIP, true, false});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 21, 21, None, 23, 23]
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

Repeated signals sample again even when the value is equal. A sample requires a
valid current value. [Input activity](../advanced-nodes/inputs.md#activity) explains
how custom nodes express similar trigger rules.

## Selecting updates by count

`take` forwards an initial number of valid ticks. `drop` skips an initial number.
`step` selects ticks at a fixed count interval. Counts advance on valid input
updates, including equal values, rather than on elapsed time. The
[tick-selection reference](../../reference/library-selection.md#take-drop-step)
gives a complete example and invalidation rules.

## Captured results

Both languages return `GraphRunResult` and retain sparse output events. Printing
a small result displays its updates as a list, with `None` at skipped positions.
See [result capture](../../reference/testing.md) for comparisons, timestamps,
large time gaps, indexing, and invalidation.
