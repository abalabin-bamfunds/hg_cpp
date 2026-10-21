# Reuse graphs across types

To preserve the input's value type without choosing one numeric type, use a type
variable. Type resolution binds the variable when graph code supplies arguments.

Prerequisites: [Graph functions](connect-calculations.md), [guarantees](guarantees.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from typing import TypeVar
from bg import TS, graph, test_graph

VALUE = TypeVar("VALUE")

@graph
def identity(value: TS[VALUE]) -> TS[VALUE]:
    return value

print(test_graph(identity[TS[int]], [18, None, 19]))
```

Output:

```text
[18, None, 19]
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
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

using VALUE = ScalarVariable<"VALUE">;
const auto identity = graph([](wp<TS<VALUE>> value) { return value; });

auto integer_identity(wp<TS<i64>> value) {
    return identity(value);
}

int main() {
    std::cout << std::boolalpha << (test_graph(integer_identity, {18, SKIP, 19}).equals({18, SKIP, 19})) << '\n';
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

Repeated uses of one scalar type variable require compatible resolved scalar
types. Numeric operators still require registered overloads; a generic signature
does not supply an implementation for every possible value type.

The Python specialization supplies the concrete input type needed to generate
test data. Ordinary graph calls infer it from their arguments. C++ can infer it
through the typed wrapper passed to `test_graph`.

A whole-time-series variable captures a complete type, including collection
shape and root guarantee. Python uses `TypeVar("SERIES", bound=TimeSeries)`;
C++ uses `TimeSeriesVariable<"SERIES">`. Use it when a graph should forward any
time series rather than only `TS` values.

Type-variable identity matters: separately created Python `TypeVar` objects
with the same name are independent. See [generic signatures](../reference/graph-code.md#generic-signatures)
for specialization, explicit mappings, and variadic collections.
