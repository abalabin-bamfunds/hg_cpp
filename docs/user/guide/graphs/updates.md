# Changing inputs

When readings or prices arrive over time, the graph can maintain the result without
your application calling each dependent calculation. To check this behavior without
a live source, give `run_graph` a finite sequence of updates for each input.

## Input sequences

To describe both updates and gaps between them, use one sequence position per
evaluation time. Positions are one microsecond apart by default; `None` in Python
and `SKIP` in C++ mean that no new input arrives at that time.

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

If a calculation combines sources that update independently, supply a separate
sequence for each input. HG retains their current values and evaluates the calculation
when its required inputs are available and an active dependency updates.

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
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace literal;

auto calibrated_temperature(auto reading, auto offset) {
    return reading + offset;
}

int main() {
    const auto result = run_graph(
        [](auto reading, auto offset) {
            const auto calibrated = calibrated_temperature(reading, offset);
            return calibrated * 10;
        },
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

If repeated equal readings should not cause more work, use `dedup` to suppress updates
whose values equal the current value. Its position in the graph determines which
calculations receive those repeated updates.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import dedup


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
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

int main() {
    const auto result = run_graph(
        [](auto reading) {
            const auto calibrated = reading + 2;
            return dedup(calibrated);
        },
        {18, 18, SKIP, 19, 18, 18});
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
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

If a dashboard should refresh on request while its underlying values keep changing,
use `sample(signal, value)`. It publishes the current value when the signal updates,
so changes to the value alone do not refresh the dashboard.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import sample


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
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

int main() {
    const auto result = run_graph(
        [](auto reading, auto signal) {
            const auto calibrated = reading + 2;
            return sample(signal, calibrated);
        },
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Repeated signals sample again even when the value is equal. A sample requires a
valid current value. [Input activity](../advanced-nodes/inputs.md#active-and-passive-inputs) explains
how custom nodes express similar trigger rules.

## Selecting updates by count

To keep only the first few updates, skip a warm-up period, or process every nth update,
use `take`, `drop`, or `step`. Their counts advance on valid input updates, including
equal values, rather than elapsed time. The
[tick-selection reference](../../reference/library-selection.md#take-drop-step)
gives a complete example and invalidation rules.

## Captured results

To inspect which updates a graph actually published, use the `GraphRunResult` returned
by `run_graph` in either language. It stores sparse output events, but prints a small
result as a list with `None` at skipped positions.
See [result capture](../../reference/testing.md) for comparisons, timestamps,
large time gaps, indexing, and invalidation.
