# Supply a shared graph input

To pass a changing offset to several graph functions, use an explicit parameter.
C++ context inputs can also request a named connection without adding it to each
intermediate call. Python currently uses explicit graph arguments for this task.

Prerequisites: [Graph functions](connect-calculations.md), [input views](input-views.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph

@graph
def calibrated(reading: int, offset: int) -> int:
    return reading + offset

@graph
def temperature(reading: int, offset: int) -> int:
    return calibrated(reading, offset)

print(test_graph(temperature, [18, 19], [2, 3]))
```

Output:

```text
[20, 22]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <iostream>
#include <bg/graph.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

const auto calibrated = node([](i64 reading, In<TS<i64>, "offset"_n, context> offset) {
    return reading + *offset;
});

auto temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return with_context("offset"_n = offset, [&] { return calibrated(reading); });
}

int main() {
    const auto result = test_graph(temperature, {18, 19}, {2, 3});
    std::cout << std::boolalpha << result.equals({20, 22}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
builds this example as `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

Both versions connect the same changing offset. The Python declaration makes
that dependency explicit. C++ `with_context` establishes a binding for graph
construction inside its scope. The context parameter names the requested
connection and its required type.

The nearest binding with that name wins. A missing or incompatible binding is
a wiring error. A named argument supplied directly for a context parameter
overrides a lexical binding. Context parameters cannot take positional arguments.
A scalar binding becomes a constant time series.

Context inputs are changing graph dependencies. They are different from
[shared scalar state](global-state.md), which does not publish time series
updates, and [services](services.md), which select shared implementations.
