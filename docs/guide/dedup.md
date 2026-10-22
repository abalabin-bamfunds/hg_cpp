# Suppress repeated values

To publish a calibrated temperature only when its value changes, pass it through
`dedup`. The calculation still adds the fixed offset of `2`; `dedup` suppresses
an update when the result equals its current output value.

## Publish changes

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import dedup


@graph
def reported_temperature(reading: int) -> int:
    calibrated = reading + 2
    return dedup(calibrated)


print(test_graph(reported_temperature, [18, 18, None, 19, 18, 18]))
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
#include <bg/testing/test_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto reported_temperature(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return dedup(calibrated);
}

int main() {
    const auto result = test_graph(reported_temperature, {18, 18, SKIP, 19, 18, 18});
    std::cout << std::boolalpha << result.equals({20, SKIP, SKIP, 21, 20, SKIP}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
also builds this example. The file is `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

| Step | Reading update | Calibrated update | Output update | Current output value |
| --- | --- | --- | --- | --- |
| 1 | 18 | 20 | 20 | 20 |
| 2 | 18 | 20 | No update | 20 |
| 3 | No update | No update | No update | 20 |
| 4 | 19 | 21 | 21 | 21 |
| 5 | 18 | 20 | 20 | 20 |
| 6 | 18 | 20 | No update | 20 |

The first value is published. At step 2, the repeated reading produces another
calibrated update of `20`, which `dedup` suppresses. At step 3, no reading arrives,
so the addition has no update to produce. Both steps leave the output at `20`.
The recorded `None` or `SKIP` means no output update, as in
[Values that change](changing-values.md#skipped-and-repeated-readings).

At step 5, the value returns to `20` after `21`. It is published again because
`dedup` compares with the current output value, not every value seen before.

## Choose where to suppress updates

To avoid recalculating calibration for repeated readings, put `dedup` before the
addition. With the imports and test from the complete example, the graph becomes:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
@graph
def reported_temperature(reading: int) -> int:
    return dedup(reading) + 2
```
:::::

:::::{tab-item} C++
:sync: cpp

```cpp
auto reported_temperature(wp<TS<i64>> reading) {
    return dedup(reading) + 2;
}
```
:::::
::::::

This version produces the same output for these readings. The first version
compares calibrated temperatures; this version compares readings and prevents
repeated readings from reaching the addition. Choose the position according to
which value's changes should trigger later calculations.

## Equality and repeated events

`dedup` uses the value's equality operation. Floating-point comparison is exact;
it does not apply a tolerance.

Suppress updates when an unchanged value carries no new information for the
calculations that follow. When every reading contributes to a count or total,
use the original input so repeated readings still contribute.

[Deduplication reference](../reference/library-selection.md#dedup) specifies the full contract.

[Temperature alarm changes](../how-to/temperature-monitor.md) applies deduplication
after a comparison to publish alarm transitions.
