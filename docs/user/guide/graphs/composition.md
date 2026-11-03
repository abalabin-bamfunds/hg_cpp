# Expressions and composition

Composition builds larger calculations from smaller ones. A named intermediate result
can feed several consumers, and a graph function can reuse a calculation with different
inputs. Both preserve the dependencies that HG uses to update results.

## Expressions

A shared intermediate result avoids repeating a calculation for each consumer.
Here, the subtotal supplies both the tax calculation and the final total.

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

Both uses of `subtotal` refer to the same output. HG evaluates the multiplication
before its consumers; each consumer sees the subtotal for that evaluation.

The library supports arithmetic, comparisons, Boolean operations, text operations,
and conversions. [Numeric reference](../../reference/library-numeric.md) specifies
supported types and numeric behavior.

## Graph functions

A graph function names a reusable calculation. Each call connects it with the supplied
arguments. This basket uses the same line-total calculation for two quantities.

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

A scalar argument configures a calculation for the complete run. The `quantity`
arguments above are fixed at construction. A connection instead supplies a value that
can change; [changing inputs](updates.md) shows that form.

Fixed configuration can also choose which calculations to connect. Native `if` works
for that choice; the [fixed-condition example](conditionals.md#fixed-conditions)
shows the declaration and call. A condition that changes during execution needs a
[conditional graph operation](conditionals.md#value-selection).
