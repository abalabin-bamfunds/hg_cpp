# Selection and validity

These operators decide which values or updates reach downstream calculations. Python
imports them from `bg.operators`; C++ uses `bg::operators` from `<bg/library.hpp>`.
Inputs can include compatible reference connections.

## valid

`valid(value)` reports root validity. It evaluates in the first cycle, accepts initially
invalid inputs, and produces a deduplicated Boolean output. Reference arguments observe
their targets through normal input bindings. Collection root validity is separate from
[item validity](input-views.md#collection-views).

(concrete-scalar-type)=
## type_ (C++)

`type_(value)` reports the concrete scalar type of a valid `TS<SCALAR>` input as
`TS<const Type *, deduplicated>`. Use it when graph code needs to observe changes
between concrete types accepted by a declared base type. Invalidation invalidates
the result. The operator is currently exposed only in C++.

```cpp
auto concrete_type = type_(value);
```

For choosing a calculation by concrete type, see [runtime dispatch](operators.md#runtime-dispatch).

## sample

`sample(signal, value)` copies the complete current value when a valid signal ticks and
`value` is valid. Changes to `value` alone do not schedule evaluation. Invalidating
either input preserves the last sampled output. The output has ordinary guarantees,
including its contained time series, so a later signal can sample an unchanged value
again. Copying uses the normal collection mutation rules: unchanged set membership and
already-valid empty dictionaries do not publish another tick. Contained references copy
reference values, not their target values. See [output copying](outputs.md).

## dedup

`dedup(value)` forwards changes through an output with at least the deduplicated
guarantee. Equality is exact and uses the scalar type's equality operation;
floating-point values use no tolerance. Inputs already observed as deduplicated, final,
or const are returned without adding a node. Invalidation and revalidation follow the
normal output rules. Collection outputs apply the guarantee recursively and retain
stronger contained guarantees.

## filter_

`filter_(condition, value)` forwards changes while the condition is valid and true. It
preserves its output while closed. Opening publishes the current value if it has updated
since the last forwarding; repeated true condition ticks do not republish an unchanged
value. Reopening reconciles the complete collection, including removals while closed.
Input invalidation propagates while open. The output retains the input's root guarantee up
to `final`. Contained time series are limited to that same guarantee because reopening
can copy unchanged items. A const input becomes final because its first forwarding can
be delayed.

## gate

`gate(condition, value, buffer_length=...)` retains input events while the condition
is false or invalid. Opening replays the oldest event immediately, then at most one
event per microsecond. Closing pauses the queue; value invalidation is retained as an
event, while condition invalidation preserves the output and queue. A valid `reset`
tick discards queued events and cancels the drain timer before handling a value tick
in the same cycle. Reset preserves the output and condition state. An open gate with
no backlog forwards directly. `buffer_length` must be positive; overflow raises an
error. The output guarantee is limited to `final`. See [retained events](library-time.md#retained-events)
for collection and reference behavior.

## if_then_else

`if_then_else(condition, true_value, false_value)` selects an input by reference.
Changing the condition exposes the selected input's current value. Updates to that input
remain observable without copying. An invalid condition or selected input makes the
observed result invalid. Updates to the other input and repeated identical condition
values do not publish additional ticks. Selection applies to the whole collection. Both
inputs must have a [common time series type](types.md#guarantees).

Select the current input for a changing condition:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import if_then_else

@graph
def chosen(condition: bool, first: int, second: int) -> int:
    return if_then_else(condition, first, second)

print(run_graph(chosen, [True, None, False, None], [10, 11, None, 12], [20, None, 21, 22]))
```

Output:

```text
[10, 11, 21, 22]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto chosen(wp<TS<bool>> condition, wp<TS<i64>> first, wp<TS<i64>> second) {
    return if_then_else(condition, first, second);
}

int main() {
    std::cout << run_graph(chosen, {true, SKIP, false, SKIP}, {10, 11, SKIP, 12}, {20, SKIP, 21, 22}) << '\n';
}
```

Output:

```text
[10, 11, 21, 22]
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

## if_

`if_(condition, value)` returns a time series bundle with `true_value` and
`false_value` reference fields. A valid condition routes `value` to the matching field
and invalidates the other field. An invalid condition invalidates both fields. Access a
field with `getattr(result, symbol("true_value"))` in Python or
`getattr(result, "true_value"_sym)` in C++. The native schema is `bg.IfResult` in Python
and `bg::operators::if_result` in C++.

Observe the true branch of a routed value:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph, symbol
from bg.operators import getattr, if_

@graph
def when_true(condition: bool, value: int) -> int:
    routed = if_(condition, value)
    return getattr(routed, symbol("true_value"))

print(run_graph(when_true, [True, False, None, True], [10, 11, 12, None]))
```

Output:

```text
[10, None, None, 12]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;
using namespace bg::literals;

auto when_true(wp<TS<bool>> condition, wp<TS<i64>> value) {
    auto routed = if_(condition, value);
    return getattr(routed, symbol{"true_value"});
}

int main() {
    std::cout << run_graph(when_true, {true, false, SKIP, true}, {10, 11, 12, SKIP}) << '\n';
}
```

Output:

```text
[10, None, None, 12]
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

## if_true

`if_true(condition, tick_once_only=False)` emits an ordinary `true` tick for each true
condition tick. False ticks produce no output. Condition invalidation preserves the last
output. It does not schedule a startup tick. When `tick_once_only` is true, the operator
becomes passive after its first true tick and emits no later ticks.

Turn true condition ticks into a signal:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import if_true

@graph
def signal(condition: bool) -> bool:
    return if_true(condition)

print(run_graph(signal, [False, True, True, False, True]))
```

Output:

```text
[None, True, True, None, True]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto signal(wp<TS<bool>> condition) {
    return if_true(condition);
}

int main() {
    std::cout << run_graph(signal, {false, true, true, false, true}) << '\n';
}
```

Output:

```text
[None, true, true, None, true]
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

## race

`race(*values)` routes the reference to the valid input whose current continuous-validity
period started first. If validity starts at the same time, the leftmost input wins. When
the selected input becomes invalid, the next earliest valid input wins. A valid-to-valid
reference retarget does not restart the input's validity period. At least one input is
required; one input is returned without a selection node. Inputs may be supplied as a
prebuilt fixed list using `race(values=values)` in Python or
`race("values"_n = values)` in C++. A positional list is one collection candidate.

Keep the earliest valid publisher until it becomes invalid:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph, INVALID
from bg.operators import race

@graph
def publisher(primary: int, backup: int) -> int:
    return race(primary, backup)

print(run_graph(publisher, [None, 10, 11, None], [20, None, INVALID, None]))
```

Output:

```text
[20, None, 11, None]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto publisher(wp<TS<i64>> primary, wp<TS<i64>> backup) {
    return race(primary, backup);
}

int main() {
    std::cout << run_graph(publisher, {SKIP, 10, 11, SKIP}, {20, SKIP, INVALID, SKIP}) << '\n';
}
```

Output:

```text
[20, None, 11, None]
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

## route_by_index

`route_by_index(index, value)` routes `value` into the selected slot of a fixed time-series
list. The result list type must be specified because its size is fixed. An index below
zero, outside the list, or invalid invalidates the previously selected slot. Changing
the index invalidates the old slot before routing the value to the new slot. The selected
slot follows its input by reference.

Specify the complete result type before calling the operator:

```python
route_by_index[TSL[TS[int], 3]](index, value)
```

```cpp
route_by_index[type<TSL<TS<i64>, 3>>](index, value)
```

Observe slot zero while the selected destination changes:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, TSL, graph, run_graph
from bg.operators import getitem, route_by_index

@graph
def slot_zero(index: int, value: int) -> int:
    routed = route_by_index[TSL[TS[int], 3]](index, value)
    return getitem(routed, 0)

print(run_graph(slot_zero, [0, 1, None, 0], [10, 11, 12, None]))
```

Output:

```text
[10, None, None, 12]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;
using namespace bg::literals;

auto slot_zero(wp<TS<i64>> index, wp<TS<i64>> value) {
    auto routed = route_by_index[type<TSL<TS<i64>, 3>>](index, value);
    return getitem(routed, 0);
}

int main() {
    std::cout << run_graph(slot_zero, {0, 1, SKIP, 0}, {10, 11, 12, SKIP}) << '\n';
}
```

Output:

```text
[10, None, None, 12]
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

## all_valid

`all_valid(value)` tests root validity and validity of every immediate child. Its
startup and deduplication rules are the same as `valid`. It does not recursively require
all descendants of a nested collection.

## default_

`default_(value, fallback)` follows `value` while it is valid and supplies the fallback
while it is invalid. It does not make `value` valid. The fallback is a scalar of the
input scalar type; a time series fallback is not accepted. Use an optional node input
for an application-specific fallback rule.

## merge

`merge(*values)` publishes the leftmost valid input that changed in the current
evaluation. A valid input that did not change does not block later inputs. A scalar
merge retains its current output when no valid changed input is available.
Inputs have a common time series type. Collection overloads merge recursively;
whole-input invalidation does not erase a previously merged collection.

The [alternative-publisher example](../guide/graphs/conditionals.md#alternative-publishers)
shows simultaneous updates and updates from a secondary publisher.

## take, drop, step

These operators select valid root input ticks. `n` is a fixed integer.

| Call | Selection | Constraint |
| --- | --- | --- |
| `take(value, n)` | First `n` valid ticks | `n >= 0` |
| `drop(value, n)` | All ticks after the first `n` valid ticks | `n >= 0` |
| `step(value, n)` | First valid tick, then every `n`th valid tick | `n > 0` |

Skipped positions and root invalidations do not advance or reset the count.
`take` propagates invalidation until its count is complete, then retains its output.
`drop` propagates invalidation after forwarding begins. `step` propagates invalidation
independently of its tick selection. Invalid counts fail during construction.

For collections, `step` copies the complete current value on each selected tick.
`take` forwards deltas. `drop` copies the current value when forwarding starts, then
forwards deltas. These operators retain supported input shapes. Delayed first
publication limits const guarantees to final.

`take(value, duration)` and `drop(value, duration)` accept a fixed nonnegative
duration instead of a tick count. C++ uses `timedelta_t`; Python uses
`datetime.timedelta`. The keyword remains `n` for either form. The first valid
input tick starts the interval. Take forwards events before its end; drop
forwards events at or after its end.
Passing the deadline alone produces no tick. Root invalidation does not reset
the interval. Take propagates invalidation only before the deadline; drop
propagates it at or after the deadline. Drop samples the complete current value when
forwarding begins, then forwards deltas. Zero duration selects no events for
take and returns the input unchanged for drop.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import step

@graph
def every_other(value: int) -> int:
    return step(value, 2)

print(run_graph(every_other, [10, 11, None, 12, 13, 14]))
```

Output:

```text
[10, None, None, 12, None, 14]
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

auto every_other(wp<TS<i64>> value) {
    return step(value, i64{2});
}

int main() {
    const auto result = run_graph(every_other, {10, 11, SKIP, 12, 13, 14});
    std::cout << result << '\n';
}
```

Output:

```text
[10, None, None, 12, None, 14]
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

Limit forwarding to three microseconds from the first valid value:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from bg import graph, run_graph
from bg.operators import take

@graph
def initial_interval(value: int) -> int:
    return take(value, timedelta(microseconds=3))

print(run_graph(initial_interval, [None, 10, 11, 12, 13, 14]))
```

Output:

```text
[None, 10, 11, 12, None, None]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;
using namespace bg::literals;

auto initial_interval(wp<TS<i64>> value) {
    return take(value, timedelta_t{3});
}

int main() {
    std::cout << run_graph(initial_interval, {SKIP, 10, 11, 12, 13, 14}) << '\n';
}
```

Output:

```text
[None, 10, 11, 12, None, None]
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

## slice_

`slice_(value, start, stop, step=1)` selects zero-based valid tick positions in
`[start, stop)`, taking every `step`th position from `start`. Bounds are fixed
nonnegative integers with `start <= stop`; `step` must be positive. An empty
range publishes nothing. Invalid arguments fail during wiring.

Slice composes `step(drop(take(value, stop), start), step)`. Root invalidation
does not consume a position. It propagates after forwarding begins and before
the slice ends; the completed slice retains its output. Collection selections
sample the complete current value, including changes made between selected
ticks.

Select a bounded range of valid input ticks:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import slice_

@graph
def window(value: int) -> int:
    return slice_(value, 1, 5, 2)

print(run_graph(window, [10, 11, None, 12, 13, 14, 15]))
```

Output:

```text
[None, 11, None, None, 13, None, None]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto window(wp<TS<i64>> value) {
    return slice_(value, 1, 5, 2);
}

int main() {
    std::cout << run_graph(window, {10, 11, SKIP, 12, 13, 14, 15}) << '\n';
}
```

Output:

```text
[None, 11, None, None, 13, None, None]
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
