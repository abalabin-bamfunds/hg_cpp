# Values that change

Connect inputs that update independently and predict the resulting outputs.
Use the setup from [Your first graph](getting-started.md). For each complete
example below, replace the contents of `first_graph.py` or `first_graph.cpp`
and run it again. Rebuild the C++ executable after each change.

## Skip an input update

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

Use `None` in a `run_graph` input sequence to skip that input's update for a
step:

```python
import bg


@bg.graph
def calibrated_temperature(reading: int) -> int:
    return reading + 2


print(bg.run_graph(calibrated_temperature, [18, None, 19, None]))
```

Output:

```text
[20, None, 21, None]
```

Here, `None` in the result means the output did not update at that step.
:::

:::{tab-item} C++
:sync: cpp

Use `literal::SKIP` in an input literal sequence to skip an update:

```cpp
#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

int main() {
    const auto calibrated_temperature = bg::node([](int reading) {
        return reading + 2;
    });

    const auto result = bg::run_graph(
        calibrated_temperature, {18, literal::SKIP, 19, literal::SKIP});
    std::cout << std::boolalpha
              << result.equals({20, literal::SKIP, 21, literal::SKIP}) << '\n';
}
```

Output:

```text
true
```

Use `literal::SKIP` in the expected results when the output did not update
at that step. The node parameter remains a plain `int`.
:::
::::

| Step | Reading update | Current reading | Output update |
| --- | --- | --- | --- |
| 1 | 18 | 18 | 20 |
| 2 | No update | 18 | No update |
| 3 | 19 | 19 | 21 |
| 4 | No update | 19 | No update |

Skipping an update preserves the current input value. The output's current
value also remains available when it does not update.

BG calls a value that can change over time a **time series**. A time series
holds its current value and last updated time. `run_graph` records output
updates so you can inspect the run.

## Connect two changing inputs

Make the calibration offset a second input:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
import bg


@bg.graph
def adjustable_temperature(reading: int, offset: int) -> int:
    return reading + offset


print(bg.run_graph(
    adjustable_temperature,
    [18, None, 19, None],
    [None, 2, None, 3],
))
```

Output:

```text
[None, 20, 21, 22]
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <iostream>

int main() {
    const auto adjustable_temperature = bg::node([](int reading, int offset) {
        return reading + offset;
    });

    const auto result = bg::run_graph(
        adjustable_temperature,
        {18, literal::SKIP, 19, literal::SKIP},
        {literal::SKIP, 2, literal::SKIP, 3});
    std::cout << std::boolalpha
              << result.equals({literal::SKIP, 20, 21, 22}) << '\n';
}
```

Output:

```text
true
```
:::
::::

These calls supply one update sequence per input, in parameter order. Each step
supplies the updates at the same position in those sequences.

| Step | Reading update | Offset update | Current reading | Current offset | Output update |
| --- | --- | --- | --- | --- | --- |
| 1 | 18 | No update | 18 | No current value | No update |
| 2 | No update | 2 | 18 | 2 | 20 |
| 3 | 19 | No update | 19 | 2 | 21 |
| 4 | No update | 3 | 19 | 3 | 22 |

The addition waits until both inputs have current values. After that, an
update to either input calculates a result using the current values of both.

A time series with a current value is **valid**. At step 1, the offset is
invalid because it has not received a value. At step 2, it becomes valid and
the addition can produce its first result.

If both inputs update in one step, the addition uses both new values and
produces one result. Replace the test call and output statement above with:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
print(bg.run_graph(adjustable_temperature, [18, 19], [2, 3]))
```

Output:

```text
[20, 22]
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
const auto result = bg::run_graph(
    adjustable_temperature,
    {18, 19},
    {2, 3});
std::cout << std::boolalpha << result.equals({20, 22}) << '\n';
```

Output:

```text
true
```
:::
::::

## An update can repeat a value

Use `calibrated_temperature` from the first example on this page. Replace its
test call and output statement with:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
print(bg.run_graph(calibrated_temperature, [18, 18, 19]))
```

Output:

```text
[20, 20, 21]
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
const auto result = bg::run_graph(calibrated_temperature, {18, 18, 19});
std::cout << std::boolalpha << result.equals({20, 20, 21}) << '\n';
```

Output:

```text
true
```
:::
::::

In these examples, a repeated value still counts as an update. Use the skip
marker in an input sequence when you intend to preserve a value without an
update.
