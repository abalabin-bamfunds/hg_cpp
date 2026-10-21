# Group values by field name

To group a fixed set of named values with potentially different types, declare
a schema and use a time series bundle, `TSB`. Each field updates independently.

Prerequisites: [Fixed lists](fixed-lists.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TSB, graph, schema, symbol, test_graph
from bg.operators import getattr

@schema
class Sensor:
    reading: int
    enabled: bool

@graph
def temperature(sensor: TSB[Sensor]) -> int:
    return getattr(sensor, symbol("reading")) + 2

print(test_graph(temperature, [{"reading": 18, "enabled": True}, {"enabled": False}, {"reading": 19}]))
```

Output:

```text
[20, None, 21]
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
#include <bg/time_series/tsb/tsb.hpp>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

template <template <typename, auto...> typename Field>
struct Sensor {
    Field<TS<i64>> reading;
    Field<TS<bool>> enabled;
};

auto temperature(wp<TSB<Sensor>> sensor) {
    return getattr(sensor, symbol("reading")) + 2;
}

int main() {
    const auto result = test_graph(temperature,
        {{{"reading", 18}, {"enabled", true}}, {{"enabled", false}}, {{"reading", 19}}});
    std::cout << std::boolalpha << result.equals({20, SKIP, 21}) << '\n';
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

The schema fixes field names and types. A test mapping supplies field updates;
omitted fields retain their current values. Updating `enabled` alone does not
update `reading`.

Python scalar fields such as `int` project to time series fields in `TSB`.
Explicit time series fields are also supported. C++ declares a schema template
whose `Field` members carry the field types. Python graph code selects a field
with `getattr` and a symbol name; C++ also supports `["reading"]`. In Python node
code, a bundle input view supports `input.reading` and `input["reading"]`.

A scalar bundle groups fields into one scalar value. `Bundle[Schema]` in Python
or `Bundle<Schema>` in C++ updates all of that value together inside `TS`.
Use `TSB` when fields need independent updates.

Schemas support single inheritance. A bundle can contain other time series
collections. See [schemas and scalar values](../reference/scalars.md#schemas)
for optional fields, scalar construction, and compatibility.
