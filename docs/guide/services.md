# Share a graph implementation

To let separate graph functions request one shared application value, declare a
reference service. Register its implementation in a service catalogue supplied
to the graph runner. Clients depend on the declaration rather than the source.

Prerequisites: [Running graphs](run-graphs.md), [guarantees](guarantees.md), [dictionaries](dictionaries.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, ServiceCatalogue, graph, reference_service, test_graph
from bg.operators import const_

@reference_service
def offset() -> TS[int]:
    pass

@graph
def offset_implementation() -> TS[int]:
    return const_(2)

catalogue = ServiceCatalogue()
catalogue.register_impl(offset, offset_implementation)

@graph
def temperature(reading: int) -> int:
    return reading + offset()

print(test_graph(temperature, [18, 19], catalogue=catalogue))
```

Output:

```text
[20, 21]
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
#include <bg/graph.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

const auto offset = reference_service<auto()->TS<i64>>{"offset"};
const auto offset_implementation = graph([] { return const_(2); });

auto temperature(wp<TS<i64>> reading) { return reading + offset(); }

bool check_service() {
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(offset, offset_implementation);
    return test_graph(temperature, GraphRunOptions{.services = &catalogue}, {18, 19}).equals({20, 21});
}

int main() {
    std::cout << std::boolalpha << (check_service()) << '\n';
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

The catalogue chooses implementations while the graph is constructed. Matching
requests share according to the service identity, resolved types, and scalar
arguments. A missing or ambiguous implementation is a wiring error.

## Choose a service shape

| Need | Declaration | Implementation receives |
| --- | --- | --- |
| Shared output without changing requests | Reference service | Scalar configuration |
| Shared result for each subscribed key | Subscription service | A set of subscribed keys |
| Request/reply calculations per client | Service | Dictionaries keyed by client id |

Subscription keys must have a final-or-stronger guarantee. A subscription
implementation returns a dictionary keyed by the requested key. A request/reply
implementation returns replies keyed by the client ids it receives. Removing a
client removes its request.

A scalar service argument can select an implementation; other registration
arguments configure that implementation. Multiservice registration allows one
graph to implement several declarations through request and reply ports.

[Services reference](../reference/services.md) describes declaration signatures,
filters, feedback implementations, and registration. Use explicit graph
arguments for ordinary local dependencies. Use a service when shared
implementation selection is part of the application contract.
