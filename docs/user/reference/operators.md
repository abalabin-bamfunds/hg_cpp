# Operators and overloads

An operator is a named graph callable with registered implementations. Use it when graph
code should choose a node or graph implementation from supplied types. Use a direct node
for one concrete calculation.

## Resolution

A call binds positional and named arguments, resolves type variables, checks
constraints, and selects a compatible overload. Exact scalar types matter; integer and
floating-point operands do not implicitly mix. Required and optional parameter contracts
also affect selection. A fixed-parameter overload is preferred over a variadic overload
before type specificity is compared.

Missing implementations, incompatible arguments, and ambiguous matches are construction
errors. A failure reports the operator, argument types, candidate signatures, and
rejection context. See [errors](../how-to/errors.md).

## Extension

C++ declares an operator prototype with `operator_` and registers implementations with
`overload`:

```cpp
#include <bg/library.hpp>

using namespace bg;

inline operator_<auto(i64) -> i64> doubled{"doubled", {"value"}};
inline overload<doubled> implementation{[](i64 value) { return value * 2; }};
```

`overload<operation, Definition>` registers an aggregate node definition.
`overload<operation>{graph_template}` registers a graph implementation.
`register_overloads<Group>()` constructs a group of overload members. Registration
retains its implementation after the registration object leaves scope; the operator
object must remain alive for its uses. Type parameters, requirements, and resolvers
refine signatures before construction.

Python currently exposes native operators rather than Python `@operator` or `@overload`
declarations. A Python wrapper can observe native overloads registered later. C++
extension modules export an operator through `bg::python::export_operator`.

See [C++ integration](../how-to/cpp-extension.md) for build and ownership rules. Do not
register a conversion overload merely to hide an incompatible numeric expression;
explicit `convert` makes the resulting value type visible.


## Runtime dispatch

Runtime dispatch selects an implementation from the concrete scalar types of valid
inputs. Use it when a declared base type accepts derived values and each concrete
type needs a different calculation. Value-based conditions use
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
from bg import Bundle, TS, compute_node, dispatch, graph, run_graph, schema

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
#include <bg/dispatch.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
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
