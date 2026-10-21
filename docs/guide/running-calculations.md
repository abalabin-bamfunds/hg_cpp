# Running calculations

To summarize readings received so far, use `sums` for their running total and
`means` for their running mean. Each received reading contributes once,
including repeated values.

## Keep a running total

`sums` adds each calibrated reading to the total:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import sums


@graph
def temperature_total(reading: int) -> int:
    calibrated = reading + 2
    return sums(calibrated)


print(test_graph(temperature_total, [None, 18, 18, None, 24, 22]))
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
#include <bg/testing/test_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto temperature_total(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return sums(calibrated);
}

int main() {
    const auto result = test_graph(temperature_total, {SKIP, 18, 18, SKIP, 24, 22});
    std::cout << std::boolalpha << result.equals({SKIP, 20, 40, SKIP, 66, 90}) << '\n';
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

The total first becomes available when a reading arrives. A skipped step keeps
the current total and produces no output update.

## Keep a running mean

`means` calculates the mean of all readings received so far. It returns a
floating-point result, including when its inputs are integers:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import means


@graph
def average_temperature(reading: int) -> float:
    calibrated = reading + 2
    return means(calibrated)


print(test_graph(average_temperature, [None, 18, 18, None, 24, 22]))
```

Output:

```text
[None, 20.0, 20.0, None, 22.0, 22.5]
```
:::::

:::::{tab-item} C++
:sync: cpp

```cpp
#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto average_temperature(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return means(calibrated);
}

int main() {
    const auto result = test_graph(average_temperature, {SKIP, 18, 18, SKIP, 24, 22});
    std::cout << std::boolalpha << result.equals({SKIP, 20.0, 20.0, SKIP, 22.0, 22.5}) << '\n';
}
```

Output:

```text
true
```
:::::
::::::

| Step | Reading update | Calibrated update | Readings received | Total update | Mean update |
| --- | --- | --- | --- | --- | --- |
| 1 | No update | No update | 0 | No update | No update |
| 2 | 18 | 20 | 1 | 20 | 20.0 |
| 3 | 18 | 20 | 2 | 40 | 20.0 |
| 4 | No update | No update | 2 | No update | No update |
| 5 | 24 | 26 | 3 | 66 | 22.0 |
| 6 | 22 | 24 | 4 | 90 | 22.5 |

At step 3, the repeated reading contributes another `20`. The mean is still
`20.0`, and the operator publishes that result again. At step 6, the mean is
`90 / 4`, or `22.5`.

## Choose which updates contribute

Each received reading has equal weight. A skipped step contributes no value;
it does not add another copy of the last reading.

Use [conditional forwarding](filter.md) before a running calculation to include
only forwarded readings. Use [sampling](sample.md) before it to include a value
on each signal update. [Deduplication](dedup.md) changes the history by removing
successive equal updates, so use it before an aggregate when only value changes
should contribute.

Each runtime calculation node owns its history. Repeated identical calls can
reuse that node. A new `test_graph` run starts with fresh history.

[Running calculation reference](../reference/library-stateful.md) specifies the full contract.
