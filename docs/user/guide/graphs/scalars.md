# Scalar collections

A scalar can hold several values. Each update replaces the complete collection.
This fits sources that publish a complete record, snapshot, or batch.

## Sequences

Python `tuple[int, ...]` and C++ `List<i64>` describe homogeneous scalar sequences.
`len_` computes the size of the current sequence.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import len_

@graph
def batch_size(prices: tuple[int, ...]) -> int:
    return len_(prices)

print(run_graph(batch_size, [(10, 20), (12, 20), (12,)]))
```

Output:

```text
[2, 2, 1]
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

auto batch_size(wp<TS<List<i64>>> prices) {
    return len_(prices);
}

int main() {
    const auto result = run_graph(batch_size, {{10, 20}, {12, 20}, {12}});
    std::cout << std::boolalpha << result.equals({2, 2, 1}) << '\n';
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

Each input supplies the whole sequence. The final update replaces two items with
one item. Python uses immutable tuples for this type; mutable `list[T]`
annotations are unsupported.

## Dictionaries

A scalar dictionary holds a complete key/value mapping. Keys and values have declared
types. `contains_` tests the current mapping for a key.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import contains_

@graph
def has_bid(quote: dict[str, int]) -> bool:
    return contains_(quote, "bid")

print(run_graph(has_bid, [{"bid": 10, "ask": 12}, {"ask": 13}]))
```

Output:

```text
[True, False]
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

auto has_bid(wp<TS<Dict<string, i64>>> quote) {
    return contains_(quote, "bid");
}

int main() {
    const auto result = run_graph(has_bid, {{{"bid", 10}, {"ask", 12}}, {{"ask", 13}}});
    std::cout << std::boolalpha << result.equals({true, false}) << '\n';
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

The second mapping replaces the first, so `bid` is absent. Omitted keys are not
retained in a scalar dictionary.

## Fixed tuples

Fixed tuples declare a type for each position. A value node can extract a position
with ordinary language operations. The [node guide](../nodes/calculations.md)
explains the node declaration used here. C++ `In<TS<...>>` receives a changing input;
`value_view()` borrows its current scalar value.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph

@node
def quantity(order: tuple[str, int]) -> int:
    return order[1]

@graph
def order_quantity(order: tuple[str, int]) -> int:
    return quantity(order)

print(run_graph(order_quantity, [("buy", 3), ("sell", 5)]))
```

Output:

```text
[3, 5]
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

const auto quantity = node([](In<TS<Tuple<string, i64>>> order) {
    return order.value_view().get<1>().value();
});

auto order_quantity(wp<TS<Tuple<string, i64>>> order) {
    return quantity(order);
}

int main() {
    const auto result = run_graph(order_quantity, {{"buy", 3}, {"sell", 5}});
    std::cout << std::boolalpha << result.equals({3, 5}) << '\n';
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

## Named fields

Schemas name fields and declare their types. `Bundle[Schema]` in Python and
`Bundle<Schema>` in C++ represent one structured scalar value. A node reads its
fields from the current record.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import Bundle, graph, node, run_graph, schema

@schema
class Quote:
    bid: int
    ask: int

@node
def spread(quote: Bundle[Quote]) -> int:
    return quote.ask - quote.bid

@graph
def quoted_spread(quote: Bundle[Quote]) -> int:
    return spread(quote)

print(run_graph(quoted_spread, [Quote(10, 12), Quote(11, 14)]))
```

Output:

```text
[2, 3]
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

template <template <typename, auto...> typename Field>
struct Quote {
    Field<i64> bid;
    Field<i64> ask;
};

const auto spread = node([](In<TS<Bundle<Quote>>> input) {
    const auto quote = input.value_view();
    return scalar_cast<i64>(quote[1]).value() - scalar_cast<i64>(quote[0]).value();
});

auto quoted_spread(wp<TS<Bundle<Quote>>> quote) {
    return spread(quote);
}

int main() {
    const auto result = run_graph(quoted_spread, {{{"bid", 10}, {"ask", 12}}, {{"bid", 11}, {"ask", 14}}});
    std::cout << std::boolalpha << result.equals({2, 3}) << '\n';
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

C++ scalar bundle views access fields in schema order; `scalar_cast` supplies each
field's declared type. Python receives the schema object with named attributes.

The [scalar reference](../../reference/scalars.md#schemas) specifies schema
construction and compatibility. C++ also supports scalar `Set<T>`; Python scalar
set annotations are not exposed.

For independently changing positions, fields, or keys, use
[time series collections](../advanced-graphs/collections.md). A node processes a
complete scalar collection with [value code](../nodes/calculations.md#scalar-collections).
