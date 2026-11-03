# Conditionals

When a calculation needs to make a choice, the operation depends on what should
change: a selected value, the updates sent downstream, or the graph instance that
runs. These forms have different effects on branch evaluation and retained state.

| Requirement | Operation | Behavior |
| --- | --- | --- |
| Fixed configuration | Native `if` | Chooses connections during construction |
| One of two changing values | `if_then_else` | Follows the selected input; both calculations remain connected |
| Two complementary destinations | [`if_`](../../reference/library-selection.md#if_) | Routes to the true or false field |
| A signal for true ticks | [`if_true`](../../reference/library-selection.md#if_true) | Emits true and ignores false ticks |
| A destination selected by number | [`route_by_index`](../../reference/library-selection.md#route_by_index) | Routes into one fixed-list slot |
| Forward while enabled | `filter_` | Forwards while open; retains output while closed |
| Retain updates while closed | `gate` | Queues input events and replays them when open |
| Collect scalar values | `batch` | Publishes queued values when opened or when its timer expires |
| A value while an input is unavailable | `default_` | Uses a fallback until the source has a current value |
| Updates from alternative publishers | `merge` | Selects the leftmost valid input that changed |
| One publisher while it remains valid | [`race`](../../reference/library-selection.md#race) | Retains the earliest continuously valid input |
| One active calculation | `switch_` | Starts the selected graph instance and stops the previous instance |

## Fixed conditions

If a fixed setting determines which calculations your graph needs, use ordinary `if`
while constructing it. The selected connections remain in place for the complete run,
so this works for configuration such as enabling a calibration step.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph


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
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;

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
    std::cout << result << '\n';
}
```

Output:

```text
[20, 22, 21]
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

A connection has no native Boolean value during construction. Python `if reading > 0`
raises a truth-conversion error; the equivalent C++ expression cannot convert to
`bool`. Node code can use native `if` because it receives values during evaluation.

## Value selection

If the choice needs to change while the graph runs, use
`if_then_else(condition, true_value, false_value)`. It follows the selected input and
exposes that input's current value whenever the condition changes.

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

auto capped_temperature(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    const auto too_hot = calibrated > 21;
    return if_then_else(too_hot, const_(21), calibrated);
}

int main() {
    const auto result = run_graph(capped_temperature, {18, 20, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 21, 21]
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

Both branch calculations are constructed and can evaluate. Selection does not
short-circuit a branch containing an error or expensive work. An update from the
unselected input does not publish a selected result. An invalid condition or selected
input makes the observed result invalid.

## Boolean combinations

To make a choice depend on several changing conditions, combine them with graph
Boolean operations. Python uses `and_`, `or_`, and `not_` because native `and`, `or`,
and `not` require an immediate Boolean value; C++ supports the corresponding
overloaded operators on connections.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import and_


@graph
def temperature_in_range(reading: int) -> bool:
    calibrated = reading + 2
    return and_(calibrated >= 20, calibrated <= 21)


print(run_graph(temperature_in_range, [17, 18, 19, 20]))
```

Output:

```text
[False, True, True, False]
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

auto temperature_in_range(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return (calibrated >= 20) && (calibrated <= 21);
}

int main() {
    const auto result = run_graph(temperature_in_range, {17, 18, 19, 20});
    std::cout << result << '\n';
}
```

Output:

```text
[false, true, true, false]
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

## Multiple outcomes

If a result has more than two possible values, nested selections can express a small
set of outcomes, such as low, normal, and high readings. Each result must have a
compatible value type.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import const_, if_then_else


@graph
def temperature_band(reading: int) -> int:
    calibrated = reading + 2
    return if_then_else(
        calibrated < 20,
        const_(-1),
        if_then_else(calibrated > 21, const_(1), const_(0)),
    )


print(run_graph(temperature_band, [17, 18, 20, 17]))
```

Output:

```text
[-1, 0, 1, -1]
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

auto temperature_band(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return if_then_else(
        calibrated < 20,
        const_(-1),
        if_then_else(calibrated > 21, const_(1), const_(0)));
}

int main() {
    const auto result = run_graph(temperature_band, {17, 18, 20, 17});
    std::cout << result << '\n';
}
```

Output:

```text
[-1, 0, 1, -1]
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

For numeric bounds, [`clip`](../../reference/library-numeric.md#clip) expresses an
inclusive lower and upper limit directly. For substantial case-specific calculations,
`switch_` gives each case a graph function.

## Conditional forwarding

If updates should reach downstream calculations only while a condition holds, use
`filter_(condition, value)`. Closing the filter keeps its last output available;
reopening it forwards the current source value if that value has changed since the
last forwarding.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import filter_


@graph
def reported_temperature(reading: int, enabled: bool) -> int:
    calibrated = reading + 2
    return filter_(enabled, calibrated)


print(run_graph(
    reported_temperature,
    [18, None, 19, 20, None, None, None, 20],
    [False, True, False, None, True, False, True, None],
))
```

Output:

```text
[None, 20, None, None, 22, None, None, 22]
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

auto reported_temperature(wp<TS<i64>> reading, wp<TS<bool>> enabled) {
    const auto calibrated = reading + 2;
    return filter_(enabled, calibrated);
}

int main() {
    const auto result = run_graph(
        reported_temperature,
        {18, SKIP, 19, 20, SKIP, SKIP, SKIP, 20},
        {false, true, false, SKIP, true, false, true, SKIP});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 20, None, None, 22, None, None, 22]
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

A repeated true condition does not republish an unchanged value, and the upstream
calculation continues to run while the filter is closed.

## Fallback values

If a result should remain available before a source arrives or after it becomes
invalid, use `default_(value, fallback)` to supply a replacement. The fallback belongs
to the derived result and does not change the source's availability.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import default_

@graph
def configured(offset: int) -> int:
    return default_(offset, 2) + 18

print(run_graph(configured, [None, 0, 3]))
```

Output:

```text
[20, 18, 21]
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

auto configured(wp<TS<i64>> offset) {
    return default_(offset, 2) + 18;
}

int main() {
    const auto result = run_graph(configured, {SKIP, 0, 3});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 18, 21]
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

A zero value is available and must not select the fallback.
[Validity](../advanced-graphs/types.md#validity) specifies absence and invalidation.

## Alternative publishers

To accept updates from several sources into one result, use `merge`. On each
evaluation it publishes the leftmost valid input that changed, so a retained primary
value does not block a new update from a secondary source.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import merge

@graph
def readings(primary: int, secondary: int) -> int:
    return merge(primary, secondary)

print(run_graph(readings, [10, None, 12, 13], [20, 21, None, 23]))
```

Output:

```text
[10, 21, 12, 13]
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

auto readings(wp<TS<i64>> primary, wp<TS<i64>> secondary) {
    return merge(primary, secondary);
}

int main() {
    const auto result = run_graph(readings, {10, SKIP, 12, 13}, {20, 21, SKIP, 23});
    std::cout << result << '\n';
}
```

Output:

```text
[10, 21, 12, 13]
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

Both inputs update at the first and last positions, so the primary wins those ties.
This differs from a fallback that always prefers an available primary value.

## Graph selection

If each mode needs its own calculation, state, or resources, use `switch_` to run the
case graph selected by a changing key. Changing to a different case stops the current
instance and starts a new one; returning to an earlier case starts it with fresh state.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, switch_, run_graph
from hgraph.operators import sums

@graph
def accumulated(reading: int) -> int:
    return sums(reading)

@graph
def current(reading: int) -> int:
    return reading

@graph
def temperature(mode: int, reading: int) -> int:
    return switch_(mode, {1: accumulated, 2: current}, reading)

print(run_graph(temperature, [1, None, 2, 1], [18, 19, 20, 21]))
```

Output:

```text
[18, 37, 20, 21]
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
#include <hg/switch.hpp>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto accumulated(wp<TS<i64>> reading) { return sums(reading).as<TS<i64>>(); }
auto current(wp<TS<i64>> reading) { return reading; }

auto temperature(wp<TS<i64>> mode, wp<TS<i64>> reading) {
    return switch_(mode, cases{case_(i64{1}, accumulated), case_(i64{2}, current)}, reading)
        .as<TS<i64>>();
}

int main() {
    const auto result = run_graph(temperature, {1, SKIP, 2, 1}, {18, 19, 20, 21});
    std::cout << result << '\n';
}
```

Output:

```text
[18, 37, 20, 21]
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

Mode 1 accumulates readings: `18`, then `37`. Mode 2 returns the current reading.
Returning to mode 1 starts a new accumulator at `21`.

An equal key keeps the current instance. `DEFAULT` handles unmatched or invalid keys;
without it, selection raises an evaluation error. Cases must have compatible output
types. The selected instance receives current arguments when it starts.

Timers and resources stop with their instance. The [lifecycle chapter](../advanced-nodes/state.md)
explains resource ownership; [switch reference](../../reference/higher-order.md#switch_)
specifies grouped keys and source/sink cases.
