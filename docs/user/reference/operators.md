# Operators and overloads

If one operation needs different implementations for different input types, define
an operator and register its node or graph implementations as overloads. A direct node
is sufficient for one concrete calculation.

## Resolution

To use an operation across supported types, call the operator and let HG select its
implementation. Resolution binds positional and named arguments, resolves type
variables, checks constraints, and selects a compatible overload. Exact scalar types
matter; integer and floating-point operands do not implicitly mix. Required and optional
parameter contracts also affect selection. A fixed-parameter overload is preferred
over a variadic overload before type specificity is compared.

Missing implementations, incompatible arguments, and ambiguous matches are construction
errors. A failure reports the operator, argument types, candidate signatures, and
rejection context. See [errors](../how-to/errors.md).

## Declaring an operator

To give callers one operation across several input types, declare an operator and
register a node or graph for each supported signature. A node overload implements a
calculation with values; a graph overload composes existing calculations. This example
uses both forms to double integer and floating-point inputs:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from typing import TypeVar
from hgraph import TS, graph, node, operator, run_graph

VALUE = TypeVar("VALUE")

@operator
def doubled(value: TS[VALUE]) -> TS[VALUE]: ...

@node(overloads=doubled)
def doubled_integer(value: int) -> int:
    return value * 2

@graph(overloads=doubled)
def doubled_float(value: float) -> float:
    return value * 2.0

@graph
def integers(value: int) -> int:
    return doubled(value)

@graph
def floats(value: float) -> float:
    return doubled(value)

print(run_graph(integers, [3, 8]))
print(run_graph(floats, [1.5, 2.5]))
```

Output:

```text
[6, 16]
[3.0, 5.0]
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

#include <hg/graph.hpp>

using namespace hg;
using namespace hg::operators;

inline operator_<auto(SCALAR) -> SCALAR> doubled{"doubled", {"value"}};
inline overload<doubled> doubled_integer{[](i64 value) { return value * 2; }};
inline overload<doubled> doubled_float{
    graph([](wp<TS<double>> value) { return value * 2.0; })};

int main() {
    std::cout << run_graph([](auto value) {
        return doubled(value);
    }, {3, 8}) << '\n';
    std::cout << run_graph([](auto value) {
        return doubled(value);
    }, {1.5, 2.5}) << '\n';
}
```

Output:

```text
[6, 16]
[3, 5]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Python `@node(overloads=operation)`, `@compute_node(overloads=operation)`, and
`@graph(overloads=operation)` register their decorated implementation. Use
`compute_node` when the implementation needs [input views](input-views.md).
For a Python-declared operator, `@operation.overload` above a node or graph decorator
registers the implementation in the same way.

C++ `overload<operation, Definition>` registers an aggregate node definition;
`overload<operation>{graph_template}` registers a graph implementation.
`register_overloads<Group>()` constructs a group of overload members. Registration
retains its implementation after the registration object leaves scope; the operator
object must remain alive for its uses.

Register implementations before graph wiring. Type parameters, requirements, and
resolvers refine signatures before construction. To expose explicit specialization
in Python, declare `@operator(type_params=(VALUE,))`; callers can then write
`operation[int](input_point)`. See [generic declarations](graph-code.md).

## Extending a library operator

If your domain needs a library operation for an additional type, register an overload
on that operator. Python's `overloads=` accepts both Python-declared operators and
native operators exported to Python. Existing native overloads remain available.

Here the application defines Boolean `add` as logical OR. This is a registered
application rule; the numeric overloads keep their existing behavior:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph
from hgraph.operators import add

@node(overloads=add)
def add_flags(lhs: bool, rhs: bool) -> bool:
    return lhs or rhs

@graph
def flags(lhs: bool, rhs: bool) -> bool:
    return add(lhs, rhs)

@graph
def numbers(lhs: int, rhs: int) -> int:
    return add(lhs, rhs)

print(run_graph(flags, [False, True, False], [True, False, False]))
print(run_graph(numbers, [2, 3], [4, 5]))
```

Output:

```text
[True, True, False]
[6, 8]
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

inline overload<operators::add> add_flags{
    [](bool lhs, bool rhs) { return lhs || rhs; }};

int main() {
    std::cout << run_graph([](auto lhs, auto rhs) {
        return add(lhs, rhs);
    }, {false, true, false}, {true, false, false}) << '\n';
    std::cout << run_graph([](auto lhs, auto rhs) {
        return add(lhs, rhs);
    }, {2, 3}, {4, 5}) << '\n';
}
```

Output:

```text
[true, true, false]
[6, 8]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Overloads participate in the same resolution rules as built-in implementations.
An equally specific duplicate is ambiguous. Registrations affect subsequent calls
to that operator, so put application-specific registrations in an explicit setup
module or translation unit.

C++ extension modules can export mutable native operators through
`hg::python::export_operator`. See [C++ integration](../how-to/cpp-extension.md)
for build and ownership rules. Use explicit `convert` for numeric type changes so
the resulting value type remains visible at the call site.

## Runtime dispatch

If a declared base type accepts derived values that need different calculations,
use runtime dispatch to select an implementation from the concrete scalar types of
valid inputs. Value-based conditions use
[`switch_`](higher-order.md#switch_) instead.

Python `@dispatch` declares a signature; `.overload` registers decorated node or graph
implementations. C++ `dispatch_<Signature>{implementations...}` supplies the contract
and implementations. The most specific applicable implementation wins.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import Bundle, TS, compute_node, dispatch, graph, run_graph, schema

@schema
class Asset:
    name: str

@schema
class Future(Asset):
    expiry: int

@dispatch
def classify(value: TS[Bundle[Asset]]) -> TS[int]: ...

@classify.overload
@compute_node
def asset(value: TS[Bundle[Asset]]) -> TS[int]:
    return 1

@classify.overload
@compute_node
def future(value: TS[Bundle[Future]]) -> TS[int]:
    return 2

@graph
def category(value: TS[Bundle[Asset]]) -> int:
    return classify(value)

print(run_graph(category, [Asset("cash"), Future("oil", 202612), Asset("cash")]))
```

Output:

```text
[1, 2, 1]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/dispatch.hpp>
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

template <template <typename, auto...> typename Field>
struct Asset {
    Field<string> name;
};

template <template <typename, auto...> typename Field>
struct Future : schema_extends<Asset, Field> {
    Field<i64> expiry;
};

int main() {
    const auto asset = node([](In<TS<Bundle<Asset>>>) { return i64{1}; });
    const auto future = node([](In<TS<Bundle<Future>>>) { return i64{2}; });
    const auto classify = dispatch_<auto(TS<Bundle<Asset>>) -> i64>{asset, future};
    const auto publish = node([](bool use_future, Scalar<Bundle<Asset>> cash,
                                 Scalar<Bundle<Future>> contract, Out<TS<Bundle<Asset>>> output) {
        if (use_future) {
            output.set(contract);
        } else {
            output.set(cash);
        }
    });

    auto memory = allocator{};
    const auto cash = bundle<Asset>(memory, "cash");
    const auto contract = bundle<Future>(memory, "oil", i64{202612});
    const auto category = [&](wp<TS<bool>> use_future) {
        return classify(publish(use_future, cash.view(), contract.view())).as<TS<i64>>();
    };
    const auto result = run_graph(category, {false, true, false});
    std::cout << result << '\n';
}
```

Output:

```text
[1, 2, 1]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Changing the runtime type selects a new inner graph instance. An equal type retains
the selected instance and forwards input changes to it. An absent applicable
implementation raises an evaluation error. Implementations must satisfy the declared
parameter and result contracts. Dispatch can select from multiple input types and
can have an outputless signature.
