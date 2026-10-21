# Values that change

To calculate a display temperature for successive readings, the graph can accept
`reading` as an input. This is the same calibration and scaling calculation from
[Graph functions](connect-calculations.md), with readings supplied during the run.

BG calls a value that can change over time a **time series**. It keeps only its
current value and last updated time. `test_graph` supplies a sequence of input
updates and records the output updates.

## A sequence of readings

Here, the reading changes while the offset remains fixed at `2`:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph


@graph
def calibrated_temperature(reading: int, offset: int) -> int:
    return reading + offset


@graph
def display_temperature(reading: int) -> int:
    calibrated = calibrated_temperature(reading, 2)
    return calibrated * 10


print(test_graph(display_temperature, [18, 20, 19]))
```

Output:

```text
[200, 220, 210]
```

`reading: int` now accepts an input connection. The addition uses `reading`
directly; `const_` is for the fixed values used in the earlier examples.
The helper's `offset` parameter still receives the integer `2`.
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::testing;

auto calibrated_temperature(wp<TS<i64>> reading, int offset) {
    return reading + offset;
}

auto display_temperature(wp<TS<i64>> reading) {
    const auto calibrated = calibrated_temperature(reading, 2);
    return calibrated * 10;
}

int main() {
    const auto result = test_graph(display_temperature, {18, 20, 19});
    std::cout << std::boolalpha << result.equals({200, 220, 210}) << '\n';
}
```

Output:

```text
true
```

`wp<TS<i64>>` declares a connection to an integer time series: `wp` is a
**wiring point**, `TS` describes the time series, and `i64` is BG's 64-bit integer
type. The graph function receives the connection during construction; the
addition uses its current value during evaluation.

The helper's `int offset` parameter still receives the fixed integer `2`.
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
also builds this example. The file is `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

`test_graph` takes one sequence for each parameter of the graph passed to it.
Each position supplies an update for one evaluation step. The result records the
output update at each step:

| Step | Reading update | Calibrated temperature | Display output update |
| --- | --- | --- | --- |
| 1 | 18 | 20 | 200 |
| 2 | 20 | 22 | 220 |
| 3 | 19 | 21 | 210 |

`display_temperature` has one parameter, so this test supplies one sequence.
The offset is a fixed argument to the inner call and needs no sequence.

## Skipped and repeated readings

To represent a step with no new reading, an input sequence uses `None` in Python
or `SKIP` in C++. The following tests use the graph definitions above; the C++
statements run inside `main`:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
print(test_graph(display_temperature, [18, None, 18, 19]))
```

Output:

```text
[200, None, 200, 210]
```
:::::

:::::{tab-item} C++
:sync: cpp

```cpp
using namespace literal;

const auto result = test_graph(display_temperature, {18, SKIP, 18, 19});
std::cout << std::boolalpha << result.equals({200, SKIP, 200, 210}) << '\n';
```

Output:

```text
true
```
:::::
::::::

| Step | Reading update | Current reading | Display output update | Current display value |
| --- | --- | --- | --- | --- |
| 1 | 18 | 18 | 200 | 200 |
| 2 | No update | 18 | No update | 200 |
| 3 | 18 | 18 | 200 | 200 |
| 4 | 19 | 19 | 210 | 210 |

Skipping an update preserves the current input and output values. In the recorded
results, `None` or `SKIP` marks the step without an output update.

A repeated reading still counts as an update. Step 3 supplies `18` again and
produces another output update of `200`.

## Two independently changing inputs

When the offset can change during a run, it becomes another input to
`display_temperature`. Passing it to `calibrated_temperature` preserves its
connection, so an offset update reaches both the addition and the multiplication:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph


@graph
def calibrated_temperature(reading: int, offset: int) -> int:
    return reading + offset


@graph
def display_temperature(reading: int, offset: int) -> int:
    calibrated = calibrated_temperature(reading, offset)
    return calibrated * 10


print(test_graph(
    display_temperature,
    [18, None, 19, None],
    [None, 2, None, 3],
))
```

Output:

```text
[None, 200, 210, 220]
```

The helper's definition is unchanged: a plain `int` parameter accepts either
a fixed scalar argument or a connection to a changing integer.
:::::

:::::{tab-item} C++
:sync: cpp

```cpp
#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::testing;
using namespace literal;

auto calibrated_temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return reading + offset;
}

auto display_temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    const auto calibrated = calibrated_temperature(reading, offset);
    return calibrated * 10;
}

int main() {
    const auto result = test_graph(
        display_temperature,
        {18, SKIP, 19, SKIP},
        {SKIP, 2, SKIP, 3});
    std::cout << std::boolalpha << result.equals({SKIP, 200, 210, 220}) << '\n';
}
```

Output:

```text
true
```

Both functions now declare `offset` as `wp<TS<i64>>` to accept a changing input,
replacing the helper's earlier `int offset` argument.
:::::
::::::

The test supplies the reading sequence first and the offset sequence second,
matching the parameter order. Positions in the two sequences refer to the same
step:

| Step | Reading update | Offset update | Current reading | Current offset | Display output update |
| --- | --- | --- | --- | --- | --- |
| 1 | 18 | No update | 18 | No current value | No update |
| 2 | No update | 2 | 18 | 2 | 200 |
| 3 | 19 | No update | 19 | 2 | 210 |
| 4 | No update | 3 | 19 | 3 | 220 |

The addition waits until both inputs have current values. After that, an update
to either input calculates a result using the current values of both. A time
series with a current value is **valid**; the offset becomes valid at step 2.

## Updates in the same step

When both inputs update in one step, the addition uses both new values and
produces one result. With the two-input graph above:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
print(test_graph(display_temperature, [18, 19], [2, 3]))
```

Output:

```text
[200, 220]
```
:::::

:::::{tab-item} C++
:sync: cpp

```cpp
const auto result = test_graph(display_temperature, {18, 19}, {2, 3});
std::cout << std::boolalpha << result.equals({200, 220}) << '\n';
```

Output:

```text
true
```
:::::
::::::

For results that depend on a threshold,
[Conditionals](conditions.md) explains how to choose results during construction
or as inputs change.
