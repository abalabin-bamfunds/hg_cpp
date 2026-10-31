# Input views and controls

Input views expose current values, validity, and update information. Use them when
plain values cannot express the calculation's input requirements.

## Update information

Python `@compute_node` passes a view for each time series parameter. C++
`In<TS<T>>` requests the corresponding view.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, compute_node, graph, run_graph

@compute_node
def reading_changed(reading: TS[int], offset: TS[int]) -> TS[bool]:
    return reading.modified

@graph
def changes(reading: int, offset: int) -> bool:
    return reading_changed(reading, offset)

print(run_graph(changes, [18, None, 18, 19], [2, 3, None, 4]))
```

Output:

```text
[True, False, True, True]
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

const auto reading_changed = node([](In<TS<i64>> reading, In<TS<i64>> offset) {
    return reading.modified();
});

auto changes(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return reading_changed(reading, offset);
}

int main() {
    const auto result = run_graph(changes, {18, SKIP, 18, 19}, {2, 3, SKIP, 4});
    std::cout << result << '\n';
}
```

Output:

```text
[true, false, true, true]
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

`modified` reports a valid update in the current evaluation. Equal-valued updates
are modified too. It is not a general invalidation detector.

| Information | Python | C++ |
| --- | --- | --- |
| Current value | `.value` | `*input` or `.value()` |
| Current value exists | `.valid` | `.valid()` |
| Updated in this evaluation | `.modified` | `.modified()` |
| Last valid update time | `.last_modified_time` | `.last_updated()` |

## Optional inputs

An optional input permits evaluation while it has no current value. Plain optional
parameters are useful when only a fallback value is required.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, node, run_graph

@node
def correct(reading: int, offset: int | None) -> int:
    return reading + (2 if offset is None else offset)

@graph
def temperature(reading: int, offset: int | None) -> int:
    return correct(reading, offset)

print(run_graph(temperature, [18, None, 19, 20], [None, 0, None, 3]))
```

Output:

```text
[20, 18, 19, 23]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>
#include <optional>

using namespace bg;
using namespace bg::operators;
using namespace literal;

const auto correct = node([](i64 reading, std::optional<i64> offset) {
    return reading + offset.value_or(2);
});

auto temperature(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return correct(reading, offset);
}

int main() {
    const auto result = run_graph(graph(temperature), {18, SKIP, 19, 20}, {SKIP, 0, SKIP, 3});
    std::cout << result << '\n';
}
```

Output:

```text
[20, 18, 19, 23]
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

Zero is available. A skipped update preserves an available offset; it does not make
that offset absent. Optionality does not schedule evaluation without another trigger.

## Optional views

Optional views retain their time series annotation. Check `.valid` or `.valid()`
before accessing the value.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import INVALID, TS, compute_node, graph, run_graph

@compute_node(valid=("reading",))
def uses_offset(reading: TS[int], offset: TS[int]) -> TS[bool]:
    return offset.valid

@graph
def offset_available(reading: int, offset: int) -> bool:
    return uses_offset(reading, offset)

print(run_graph(offset_available, [18, None, None, None], [None, 0, INVALID, 3]))
```

Output:

```text
[False, True, False, True]
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

const auto uses_offset = node([](In<TS<i64>> reading, In<TS<i64>, optional> offset) {
    return offset.valid();
});

auto offset_available(wp<TS<i64>> reading, wp<TS<i64>> offset) {
    return uses_offset(reading, offset);
}

int main() {
    std::cout << run_graph(offset_available, {18, SKIP, SKIP, SKIP}, {SKIP, 0, INVALID, 3}) << '\n';
}
```

Output:

```text
[false, true, false, true]
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

Python `valid=(...)` identifies the inputs requiring validity; unselected inputs
become optional unless selected by `all_valid`. C++ states the constraint in `In<>`.
For a collection, `all_valid` requires the root and every immediate item to be valid.
The [complete-item example](../../reference/input-views.md#requiring-complete-items)
shows a partial input becoming complete, losing an item value, and recovering.

## Activity

An active input schedules evaluation when it changes. A passive input supplies its
current state when another input or a timer schedules evaluation.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, compute_node, graph, run_graph

@compute_node(active=("request",))
def snapshot(request: TS[bool], reading: TS[int]) -> TS[int]:
    return reading.value

@graph
def requested(request: bool, reading: int) -> int:
    return snapshot(request, reading)

print(run_graph(requested, [True, None, True], [18, 19, None]))
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

using namespace bg;
using namespace bg::operators;
using namespace literal;

const auto snapshot = node([](In<TS<bool>> request, In<TS<i64>, passive> reading) {
    return *reading;
});

auto requested(wp<TS<bool>> request, wp<TS<i64>> reading) {
    return snapshot(request, reading);
}

int main() {
    const auto result = run_graph(requested, {true, SKIP, true}, {18, 19, SKIP});
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
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

The reading update at position 2 changes its retained value without evaluating the
snapshot. The next request reads `19`. A required passive input still prevents
evaluation while invalid. Activity and validity are independent controls.

## View lifetime

Views and their iterators expire when the callback returns. Store owning values in
[state](state.md) when data must survive. [Input reference](../../reference/input-views.md)
lists collection access and ownership rules.
