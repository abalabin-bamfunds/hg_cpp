# Guarantees and advanced type matching

If a calculation relies on a value staying fixed or on repeated values being
suppressed, express that requirement as a time series guarantee. Generic signatures
let the same calculation work across value types while preserving these publication
contracts.

## Publication guarantees

Use a publication guarantee when the number or kind of allowed updates matters to
your calculation. HG checks that contract rather than changing updates to fit it:
for example, a final annotation rejects a second publication instead of filtering it.

| Guarantee | Contract |
| --- | --- |
| Ordinary | Equal-valued updates and repeated publications are permitted |
| Deduplicated | An equal requested mutation is suppressed |
| Final | At most one value update; the value may remain unavailable |
| Const | One value update at the time the time series starts |

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, final, graph, run_graph
from hgraph.operators import const_

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto configured(wp<TS<i64, TimeSeriesGuarantee::final>> offset) {
    return offset + 18;
}

auto example() {
    return configured(const_(i64{2}).as<TS<i64, TimeSeriesGuarantee::final>>());
}

int main() {
    const auto result = run_graph(example);
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

`dedup` constructs a deduplicated result when repeated values should be suppressed.
A fixed `const_` source publishes at its start. Collection guarantees also constrain
contained time series. References have a separate guarantee for changing targets.

## Guarantee matching

To accept only sources with the publication behavior your node relies on, declare
the required input guarantee. A weaker requirement can accept a stronger source;
omitting an explicit guarantee lets normal type resolution preserve the source's
guarantee where supported.

Operations infer output guarantees from their inputs and publication behavior.
Retargeting can cause repeated observed values even when individual targets are
final. [Type reference](../../reference/types.md#guarantees) specifies propagation.

## Generic signatures

If a calculation works for several value types, use a type variable rather than
defining the graph separately for each one. HG binds it to a concrete type from
the supplied arguments, and repeated uses of the variable require those arguments
to agree.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from typing import TypeVar
from hgraph import TS, graph, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/graph.hpp>

using namespace hg;
using namespace hg::operators;
using namespace literal;

using VALUE = ScalarVariable<"VALUE">;
const auto identity = graph([](wp<TS<VALUE>> value) { return value; });

auto integer_identity(wp<TS<i64>> value) {
    return identity(value).as<TS<i64>>();
}

int main() {
    const auto result = run_graph(graph(integer_identity), {18, SKIP, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[18, None, 19]
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

A generic signature does not create an implementation for unsupported operations.
Arithmetic still requires a matching registered operator overload.
Whole-time-series variables can preserve collection shape and guarantees as well
as scalar value type.

## Variadic parameters

When callers need to supply different numbers of inputs, use variadic parameters
to capture and forward their graph-code arguments. The graph constructs connections
for the argument list supplied at each call.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import SIZE, TS, TSL, compute_node, graph, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <hg/time_series/tsl/tsl.hpp>

using namespace hg;
using namespace hg::operators;
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
    std::cout << result << '\n';
}
```

Output:

```text
[20, 22]
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

Variadic capture occurs during construction. It does not iterate over items arriving
at runtime. [Graph signature reference](../../reference/graph-code.md#variadic-parameters)
defines capture and forwarding.

## Operators and runtime dispatch

If one operation needs different implementations for different types, use an
operator to select a registered implementation. C++ extensions can define prototypes
and overloads, and Python graph code can call exported operators.
The [operator reference](../../reference/operators.md) provides declaration syntax
and a [complete dispatch example](../../reference/operators.md#runtime-dispatch).

When a declared base type accepts derived values that need different calculations,
runtime dispatch selects an implementation from their concrete scalar types.
For choices based on values, use [conditionals](../graphs/conditionals.md).
