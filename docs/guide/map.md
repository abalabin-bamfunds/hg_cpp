# Calculate separately for each key

To apply a graph independently to each sensor, use `map`. Each active key owns
a graph instance with its own output, state, and lifecycle.

Prerequisites: [Dictionaries](dictionaries.md), [graph functions](connect-calculations.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import REMOVE, TS, TSD, graph, map, test_graph

@graph
def calibrate(reading: int) -> int:
    return reading + 2

@graph
def sensor_one(readings: TSD[int, TS[int]]) -> int:
    return map(calibrate, readings)[1]

print(test_graph(sensor_one, [{1: 18, 2: 30}, {2: 31}, {1: REMOVE}, {1: 19}]))
```

Output:

```text
[20, None, None, 21]
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
#include <bg/map.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto calibrate(wp<TS<i64>> reading) { return reading + 2; }

auto sensor_one(wp<TSD<i64, TS<i64>>> readings) {
    return map(calibrate, readings)[i64{1}].as<TS<i64>>();
}

int main() {
    std::cout << std::boolalpha << (test_graph(sensor_one, {{{1, 18}, {2, 30}}, {{2, 31}}, {{1, REMOVE}}, {{1, 19}}}).equals({20, SKIP, INVALID, 21})) << '\n';
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

`map` returns a dictionary of branch outputs. Adding a key starts its branch;
removing it stops the branch and removes its output. Re-adding the key creates
a fresh instance. A value update preserves that key's instance and state.

C++ `.as<TS<i64>>()` supplies the known selected result type when higher-order
wiring returns an untyped connection. It does not convert a scalar value.

## Share an argument across branches

Wrap a changing argument in `pass_through(offset)` to pass the complete time
series to every branch. An unwrapped dictionary is mapped by key. Scalars are
fixed arguments shared by all branches.

With multiple mapped dictionaries, the default key set is their union. A branch
can have invalid inputs for keys missing from one dictionary. `__key_set_arg__`
selects one mapped argument's keys instead. `__key_arg__` identifies the branch
parameter that receives its key as a const time series; use a position or name.

The key argument and key-set argument use the same options in Python; C++ uses
named arguments such as `"__key_arg__"_n = 0`. See
[higher-order reference](../reference/higher-order.md#map) for selection and
source/sink forms. Use [mesh](mesh.md) when one key depends on another key's result.
