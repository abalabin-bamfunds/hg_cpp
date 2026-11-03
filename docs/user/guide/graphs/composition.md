# Expressions and composition

As calculations grow, you can share intermediate results and group related expressions
into graph functions. This lets you reuse both results and calculation definitions
while HG keeps track of the dependencies between them.

## Expressions

If several calculations need the same intermediate result, assign it to a name and
use that output wherever it is needed. In this example, the subtotal supplies both
the tax calculation and the final total, so its multiplication is shared.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import const_


@graph
def order_total() -> float:
    price = const_(5.0)
    subtotal = price * 3.0
    tax = subtotal * 0.2
    return subtotal + tax + 2.0


print(run_graph(order_total))
```

Output:

```text
[20.0]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;

auto order_total() {
    const auto price = const_(5.0);
    const auto subtotal = price * 3.0;
    const auto tax = subtotal * 0.2;
    return subtotal + tax + 2.0;
}

int main() {
    const auto result = run_graph(order_total);
    std::cout << result << '\n';
}
```

Output:

```text
[20]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

HG evaluates the subtotal's multiplication before its consumers, so both receive
the result for the current evaluation.

The library supports arithmetic, comparisons, Boolean operations, text operations,
and conversions. [Numeric reference](../../reference/library-numeric.md) specifies
supported types and numeric behavior.

## Graph functions

If you need the same calculation with different inputs, define a graph function and
call it for each set of arguments. This basket reuses a line-total calculation for
two quantities, with each call adding its expressions to the basket's graph.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import const_


@graph
def line_total(price: int, quantity: int) -> int:
    return const_(price) * quantity


@graph
def basket_total() -> int:
    return line_total(5, 3) + line_total(2, 4)


print(run_graph(basket_total))
```

Output:

```text
[23]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <iostream>

using namespace hg;
using namespace hg::operators;

auto line_total(int price, int quantity) {
    return const_(price) * quantity;
}

auto basket_total() {
    return line_total(5, 3) + line_total(2, 4);
}

int main() {
    const auto result = run_graph(basket_total);
    std::cout << result << '\n';
}
```

Output:

```text
[23]
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Each call adds its calculations to the current graph. `run_graph` starts the complete
basket once; ordinary graph-function calls do not create separate runtime graphs.
[Iteration](iteration.md) covers independent graph instances for changing keys.

## Fixed parameters

For settings that stay the same throughout a run, pass a scalar argument such as the
`quantity` values above. When an argument needs to change, pass a connection instead;
[changing inputs](updates.md) shows how that preserves the dependency on its source.

Fixed configuration can also choose which calculations to connect. Native `if` works
for that choice; the [fixed-condition example](conditionals.md#fixed-conditions)
shows the declaration and call. A condition that changes during execution needs a
[conditional graph operation](conditionals.md#value-selection).
