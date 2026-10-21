# Convert numeric values

To combine integer readings with floating-point calculations, convert the
integer time series explicitly. Numeric overloads require exact operand types.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, graph, test_graph
from bg.operators import convert

@graph
def scaled(reading: int) -> float:
    return convert[TS[float]](reading) * 0.5

print(test_graph(scaled, [18, None, 19]))
```

Output:

```text
[9.0, None, 9.5]
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

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto scaled(wp<TS<i64>> reading) {
    return convert[type<TS<double>>](reading) * 0.5;
}

int main() {
    const auto result = test_graph(scaled, {18, SKIP, 19});
    std::cout << std::boolalpha << result.equals({9.0, SKIP, 9.5}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [first program's CMake project](../guide/getting-started.md#write-and-run-the-example)
builds this example as `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

The explicit result is a full time series type, not just the scalar type.
Conversion changes values during evaluation. C++ `.as<TS<double>>()` only checks
or supplies the declared connection type; it does not convert integer values.

Built-in conversions support integer to double and double to integer. Conversion
to integer truncates toward zero. NaN raises a domain error; infinity and values
outside the integer range raise an overflow error. Large integers can lose
precision when converted to floating point. See
[conversion reference](../reference/library-numeric.md#convert).
