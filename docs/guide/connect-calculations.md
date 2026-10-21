# Graph functions

To use a calculation in several places, define a graph function with parameters.
The caller supplies arguments and uses the returned output in further expressions.
Here, calibration has its own function, called by `display_temperature`.

## Parameters and calls

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import const_


@graph
def calibrated_temperature(reading: int, offset: int) -> int:
    return const_(reading) + offset


@graph
def display_temperature() -> int:
    calibrated = calibrated_temperature(18, 2)
    return calibrated * 10


print(test_graph(display_temperature))
```

Output:

```text
[200]
```

`@graph` makes `calibrated_temperature` callable from another graph function.
Its `int` annotations describe the argument and result value types.

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

auto calibrated_temperature(int reading, int offset) {
    return const_(reading) + offset;
}

auto display_temperature() {
    const auto calibrated = calibrated_temperature(18, 2);
    return calibrated * 10;
}

int main() {
    const auto result = test_graph(display_temperature);
    std::cout << std::boolalpha << result.equals({200}) << '\n';
}
```

Output:

```text
true
```

A C++ graph function is an ordinary function that connects calculations.
Here, both parameters are plain integers. `auto` infers the returned connection's
type from the expression.

:::

:::{tab-item} Build and run

This example uses the CMake project and `bg::library` link target from
[A first BG program](getting-started.md#write-and-run-the-example), with the
code saved as `first_graph.cpp`. The build and run commands are the same.
:::
::::

:::::
::::::

`calibrated_temperature(18, 2)` passes `18` as `reading` and `2` as `offset`.
Inside the function, `const_(reading)` provides the reading as a fixed graph
value. The returned addition's output becomes the multiplication's input.

These are **scalar arguments**: fixed values supplied during graph construction.
Another call can supply different values without changing the function definition.

## Graph construction and evaluation

`test_graph(display_temperature)` calls `display_temperature` to construct the
graph. Its call to `calibrated_temperature` adds the calibration calculation.
BG then evaluates the connected addition and multiplication to produce `200`.

When readings arrive during a run, [Values that change](changing-values.md)
covers input parameters and the sequences supplied to `test_graph`.

[Graph declaration reference](../reference/graph-code.md) specifies the full contract.
