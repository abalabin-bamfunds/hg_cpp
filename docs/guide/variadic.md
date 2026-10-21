# Accept a variable number of inputs

To write a calculation with a caller-selected number of homogeneous inputs,
capture them as a variadic time series list. The callback receives one list
view with a size resolved during graph construction.

Prerequisites: [Generic signatures](generics.md), [fixed lists](fixed-lists.md), [input views](input-views.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import SIZE, TS, TSL, compute_node, graph, test_graph

@compute_node
def total(*values: TSL[TS[int], SIZE]) -> TS[int]:
    return sum(item.value for item in values)

@graph
def combined(left: int, right: int) -> int:
    return total(left, right)

print(test_graph(combined, [18, 19], [2, 3]))
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
#include <bg/time_series/tsl/tsl.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

const auto total = node([](In<TSL<TS<i64>, SIZE>, var_args> values) {
    auto result = i64{0};
    for (auto index = std::size_t{0}; index < values.size(); ++index) {
        result += *values[index];
    }
    return result;
});

auto combined(wp<TS<i64>> left, wp<TS<i64>> right) { return total(left, right); }

int main() {
    std::cout << std::boolalpha << (test_graph(combined, {18, 19}, {2, 3}).equals({20, 22})) << '\n';
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

`SIZE` resolves to two in this call. The node receives one materialized list;
it does not receive expanded callback arguments. Use inputs that have current
values before reading each child's value.

A TSB capture supports different field types. Positional capture uses field
positions; named capture uses supplied argument names. Scalars in a TSB capture
become constant time series. TSL capture requires homogeneous time series and
does not accept scalar items.

Forwarding a captured pack preserves its arguments and per-argument optional
annotations. C++ `args(bundle)` expands positions and `kwargs(bundle)` expands
names; Python's decorated variadic graph bodies receive the captured collection
rather than an ordinary Python tuple or dictionary. See
[variadic reference](../reference/graph-code.md#variadic-parameters).
