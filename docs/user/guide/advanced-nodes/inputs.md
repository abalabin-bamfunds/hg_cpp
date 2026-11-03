# Input validity and activity

If a node needs to know which input changed, handle a missing value, or choose which
updates trigger evaluation, use input views and input controls. A plain value
parameter remains enough when the calculation only needs the current value.

Each time series input has two independent controls. **Validity** determines whether
the node has enough data to evaluate; **activity** determines which input changes
schedule it. Inputs are required and active by default.

| If the calculation needs… | Validity constraint | Effect |
| --- | --- | --- |
| A current value before it can run | [Required](#required-inputs) | Wait for this input to be valid |
| To handle a missing value itself | [Optional](#optional-inputs) | Allow evaluation while this input is invalid |
| Every field or item of a collection | [`all_valid`](#requiring-complete-items) | Wait for the root and every immediate item to be valid |

| If an input should… | Activity | Effect |
| --- | --- | --- |
| Trigger the calculation on updates | [Active](#active-and-passive-inputs) | Schedule evaluation when it changes |
| Supply its latest value on another trigger | [Passive](#active-and-passive-inputs) | Keep its current state without scheduling the node |

For example, a request can be active while a reading is passive. The reading's
validity constraint then determines whether a request needs an available reading
to produce a result or can use a fallback.

## Update information

To handle a new event differently from a value retained from an earlier evaluation,
request an input view and inspect its update information. Python `@compute_node`
passes a view for each time series parameter, while C++ requests one with `In<TS<T>>`.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, compute_node, graph, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

`modified` reports a change in the current evaluation, including an equal-valued
update or invalidation. Check `valid` before reading an optional input's value,
even when it is modified.

| Information | Python | C++ |
| --- | --- | --- |
| Current value | `.value` | `*input` or `.value()` |
| Current value exists | `.valid` | `.valid()` |
| Updated in this evaluation | `.modified` | `.modified()` |
| Last change time, including invalidation | `.last_modified_time` | `.last_updated()` |

## Required inputs

If a calculation cannot produce a result without an input, keep that input required.
HG checks its validity before calling the node, so the callback can read its current
value without an absence check. Both inputs in `reading_changed` above use this default.
The [required-input example](../nodes/results.md#required-inputs) shows one input
arriving before the other.

Python uses a plain non-optional parameter on `@node` or an ordinary time series
parameter on `@compute_node`. To select required inputs explicitly, use
`valid=("reading", "offset")`. C++ uses `In<TS<i64>>`, or
`In<TS<i64>, required>` to spell out the constraint.

A required collection input checks the root's validity; its items may still be
unavailable. Use [`all_valid`](#requiring-complete-items) when the calculation needs
every immediate item. A skipped update retains a current value, while invalidation
removes it and normally invalidates the node's output without calling the ordinary
evaluation callback. See [invalidation handling](../../reference/nodes.md#scheduling-and-validity).

## Optional inputs

If a node can calculate without one of its inputs, make that input optional so its
absence does not block evaluation. A plain optional parameter works well when you
only need to substitute a fallback value.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, node, run_graph

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
#include <hg/library.hpp>
#include <hg/graph.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>
#include <optional>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Zero is an available value, and skipping an update keeps any offset already supplied.
Making an input optional allows evaluation while it is absent, but does not schedule
the node without another trigger.

## Optional views

If the node also needs update information from an optional input, use an optional
view. It keeps the time series annotation and exposes validity, which you must check
with `.valid` or `.valid()` before reading the value.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import INVALID, TS, compute_node, graph, run_graph

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
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
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Python `valid=(...)` identifies the inputs requiring validity; unselected inputs
become optional unless selected by `all_valid`. `valid=()` makes every time series
input optional. C++ states the constraint on each `In<>`.

The offset remains active in this example: its arrival and invalidation both
evaluate the node, which reports its availability. Optionality allows that absence
to reach the callback; activity determines whether its changes schedule the callback.

## Requiring complete items

If a calculation needs a complete record or every item in a collection, use
`all_valid` so it does not run on partial data. For example, a total of two sensor
channels should wait until both channels have readings, then stop publishing a
total if either reading becomes unavailable.

Python selects the collection with `all_valid=("values",)`; C++ declares
`In<TSL<TS<i64>, 2>, hg::all_valid>`. This is an input constraint, distinct from the
[`all_valid(value)` operator](../../reference/library-selection.md#all_valid), which
produces a Boolean availability result.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import INVALID, TS, TSL, compute_node, graph, run_graph

@compute_node(all_valid=("values",))
def total(values: TSL[TS[int], 2]) -> TS[int]:
    return sum(item.value for item in values)

@graph
def complete_total(values: TSL[TS[int], 2]) -> int:
    return total(values)

print(run_graph(complete_total, [{0: 10}, {1: 20}, {0: INVALID}, {0: 5}]))
```

Output:

```text
[None, 30, None, 25]
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

const auto total = node([](In<TSL<TS<i64>, 2>, hg::all_valid> values) {
    return *values[0] + *values[1];
});

auto complete_total(wp<TSL<TS<i64>, 2>> values) {
    return total(values);
}

int main() {
    const auto result = run_graph(complete_total, {{{0, 10}}, {{1, 20}}, {{0, INVALID}}, {{0, 5}}});
    std::cout << result << '\n';
}
```

Output:

```text
[None, 30, None, 25]
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

The first partial update makes the list root valid, but item 1 is still unavailable,
so the total waits. The next update completes the input and publishes `30`.
Invalidating item 0 invalidates the total; supplying its new value allows `25`.

For a fixed list, every position must be valid; for a dictionary, every current
item must be valid. A valid empty dictionary passes; the constraint does not
require a particular set of keys. It checks immediate items rather than every
descendant of a nested collection. For a scalar time series, which has no item
time series, it has the same validity requirement as `required`.

`all_valid` does not select which inputs are active. On a passive collection,
item completeness is checked when another event schedules evaluation. See
[active and passive inputs](#active-and-passive-inputs).

(activity)=
## Active and passive inputs

If a node should respond to requests while retaining the latest reading, make the
request input active and the reading passive. Active input updates schedule
evaluation; a passive input supplies its current state when another input or a
timer schedules the node.

Python `active=("request",)` makes `request` active and every other time series
input passive. Omitting `active` keeps all inputs active; `active=()` selects none,
so input changes alone cannot run the node. C++ declares `active` or `passive` on
each `In<>`, with `active` as the default.

The two versions below differ only in whether the passive reading is required.
The first publishes only when a reading exists; the second can answer an earlier
request with `-1` to indicate that no reading is available.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, compute_node, graph, run_graph

@compute_node(active=("request",))
def snapshot(request: TS[bool], reading: TS[int]) -> TS[int]:
    return reading.value

@compute_node(active=("request",), valid=("request",))
def snapshot_with_default(request: TS[bool], reading: TS[int]) -> TS[int]:
    return reading.value if reading.valid else -1

@graph
def requested(request: bool, reading: int) -> int:
    return snapshot(request, reading)

@graph
def requested_with_default(request: bool, reading: int) -> int:
    return snapshot_with_default(request, reading)

requests = [True, None, True, None, False]
readings = [None, 18, None, 19, None]
print(run_graph(requested, requests, readings))
print(run_graph(requested_with_default, requests, readings))
```

Output:

```text
[None, None, 18, None, 19]
[-1, None, 18, None, 19]
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

const auto snapshot = node([](In<TS<bool>> request, In<TS<i64>, passive> reading) {
    return *reading;
});

const auto snapshot_with_default = node([](In<TS<bool>> request, In<TS<i64>, optional, passive> reading) {
    return reading.valid() ? *reading : -1;
});

auto requested(wp<TS<bool>> request, wp<TS<i64>> reading) {
    return snapshot(request, reading);
}

auto requested_with_default(wp<TS<bool>> request, wp<TS<i64>> reading) {
    return snapshot_with_default(request, reading);
}

int main() {
    std::cout << run_graph(requested, {true, SKIP, true, SKIP, false}, {SKIP, 18, SKIP, 19, SKIP}) << '\n';
    std::cout << run_graph(requested_with_default,
                          {true, SKIP, true, SKIP, false}, {SKIP, 18, SKIP, 19, SKIP}) << '\n';
}
```

Output:

```text
[None, None, 18, None, 19]
[-1, None, 18, None, 19]
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

The first request has no reading, so only the optional version publishes. Readings
at positions 2 and 4 update the retained value without scheduling either node; each
later request reads the latest one. The final request is false, but its update still
triggers evaluation: activity responds to updates regardless of their Boolean value.

| Combination | Use when… |
| --- | --- |
| Required and active | Each source update should drive the calculation, which needs a current value |
| Optional and active | The callback should process updates and availability changes even without a current value |
| Required and passive | Another trigger should read this input, but the value must exist |
| Optional and passive | Another trigger should read this input and handle its absence |
| `all_valid` and active | Collection changes should drive a calculation that needs every immediate item |
| `all_valid` and passive | Another trigger should read a complete collection |

Making an input passive changes scheduling, not whether its producer runs or whether
its value is required. Required inputs still gate evaluation scheduled by another
input or a timer. A blocked attempt is not queued as a pending request: a later
passive update alone does not retry it. For library graph code, use
[`sample`](../../reference/library-selection.md#sample) when it already provides the
needed trigger-and-reading behavior.

## View lifetime

To keep data for a later evaluation, copy it into an owning value in
[state](state.md). Views and their iterators expire when the callback returns;
the [input reference](../../reference/input-views.md) covers collection access and
ownership rules.
