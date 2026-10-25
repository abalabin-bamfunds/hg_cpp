# Guarantees and advanced type matching

A time series guarantee constrains publication. A generic signature reuses a
calculation across types while retaining type checks.

## Publication guarantees

| Guarantee | Contract |
| --- | --- |
| Ordinary | Equal-valued updates and repeated publications are permitted |
| Deduplicated | An equal requested mutation is suppressed |
| Final | At most one value update; the value may remain unavailable |
| Const | One value update at the time the time series starts |

A guarantee is a contract. Adding a final annotation does not filter later updates;
a violating publication fails.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, final, graph, run_graph
from bg.operators import const_

@graph
def configured(offset: TS[int, final]) -> int:
    return offset + 18

@graph
def example() -> int:
    return configured(const_(2))

print(run_graph(example))
```

Output:

```text
[20]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto configured(wp<TS<i64, TimeSeriesGuarantee::final>> offset) {
    return offset + 18;
}

auto example() {
    return configured(const_(i64{2}).as<TS<i64, TimeSeriesGuarantee::final>>());
}

int main() {
    const auto result = run_graph(example);
    std::cout << std::boolalpha << result.equals({20}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

`dedup` constructs a deduplicated result when repeated values should be suppressed.
A fixed `const_` source publishes at its start. Collection guarantees also constrain
contained time series. References have a separate guarantee for changing targets.

## Guarantee matching

An input requirement must accept the source guarantee. A weaker requirement can
accept a stronger source. Omitting an explicit input guarantee lets normal type
resolution preserve the source guarantee where supported.

Operations infer output guarantees from their inputs and publication behavior.
Retargeting can cause repeated observed values even when individual targets are
final. [Type reference](../../reference/types.md#guarantees) specifies propagation.

## Generic signatures

A type variable binds to a concrete type when graph arguments are supplied. Repeated
uses of one variable constrain those arguments together.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from typing import TypeVar
from bg import TS, graph, run_graph

VALUE = TypeVar("VALUE")

@graph
def identity(value: TS[VALUE]) -> TS[VALUE]:
    return value

print(run_graph(identity[TS[int]], [18, None, 19]))
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
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/graph.hpp>

using namespace bg;
using namespace bg::operators;
using namespace literal;

using VALUE = ScalarVariable<"VALUE">;
const auto identity = graph([](wp<TS<VALUE>> value) { return value; });

auto integer_identity(wp<TS<i64>> value) {
    return identity(value).as<TS<i64>>();
}

int main() {
    const auto result = run_graph(graph(integer_identity), {18, SKIP, 19});
    std::cout << std::boolalpha << result.equals({18, SKIP, 19}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

A generic signature does not create an implementation for unsupported operations.
Arithmetic still requires a matching registered operator overload.
Whole-time-series variables can preserve collection shape and guarantees as well
as scalar value type.

## Variadic parameters

Variadic parameters accept a variable number of graph-code arguments. The graph
constructs connections for the supplied argument list.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import SIZE, TS, TSL, compute_node, graph, run_graph

@compute_node
def total(*values: TSL[TS[int], SIZE]) -> TS[int]:
    return sum(item.value for item in values)

@graph
def combined(left: int, right: int) -> int:
    return total(left, right)

print(run_graph(combined, [18, 19], [2, 3]))
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
#include <bg/run_graph.hpp>
#include <iostream>
#include <bg/time_series/tsl/tsl.hpp>

using namespace bg;
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
    const auto result = run_graph(combined, {18, 19}, {2, 3});
    std::cout << std::boolalpha << result.equals({20, 22}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Variadic capture occurs during construction. It does not iterate over items arriving
at runtime. [Graph signature reference](../../reference/graph-code.md#variadic-parameters)
defines capture and forwarding.

## Operators and runtime dispatch

An operator selects a registered implementation from the supplied types. C++ extensions
can define prototypes and overloads; Python graph code can call exported operators.
[Operator reference](../../reference/operators.md) provides declaration syntax and a
[complete dispatch example](../../reference/operators.md#runtime-dispatch).

Runtime dispatch selects an implementation using concrete runtime scalar types.
It is useful for hierarchical values whose declared input type admits derived values.
Value-based cases belong to [conditionals](../graphs/conditionals.md).
