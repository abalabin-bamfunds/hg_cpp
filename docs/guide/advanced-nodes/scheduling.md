# Time, scheduling, and sources

A scheduled node can evaluate at a later time without another input update.
Sources introduce values from timers, finite sequences, or external producers.

## Evaluation time

The evaluation clock identifies the time of the work being processed. An input's
last update time can be older than the current evaluation time.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import EvaluationClock, graph, node, run_graph

@node
def microsecond(reading: int, clock: EvaluationClock = None) -> int:
    return clock.evaluation_time.microsecond

@graph
def timestamps(reading: int) -> int:
    return microsecond(reading)

print(run_graph(timestamps, [18, None, 19]))
```

Output:

```text
[1, None, 3]
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
#include <bg/evaluation_context.hpp>

using namespace bg;
using namespace bg::operators;
using namespace literal;

struct Microsecond {
    In<TS<i64>> reading;
    Out<TS<i64>> output;

    void evaluate(EvaluationContext* context) const {
        output.set(context->evaluation_time().time_since_epoch().count());
    }
};

const auto microsecond = node<Microsecond>();

auto timestamps(wp<TS<i64>> reading) {
    return microsecond(reading);
}

int main() {
    const auto result = run_graph(graph(timestamps), {18, SKIP, 19});
    std::cout << std::boolalpha << result.equals({1, SKIP, 3}) << '\n';
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

Default simulation input positions start one microsecond after the epoch and are
one microsecond apart. Python uses `EvaluationClock`; a C++ aggregate receives
`EvaluationContext` in its callback.

## Scheduled sources

A source with no changing input can publish at requested evaluation times. A finite
source makes simulation termination explicit.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from bg import TS, generator, graph, run_graph

@generator
def readings() -> TS[int]:
    yield timedelta(microseconds=1), 18
    yield timedelta(microseconds=1), 19

@graph
def temperature() -> int:
    return readings() + 2

print(run_graph(temperature))
```

Output:

```text
[None, 20, 21]
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
#include <bg/evaluation_context.hpp>

using namespace bg;
using namespace bg::operators;
using namespace literal;

struct Readings {
    static constexpr bool uses_timed_scheduling = true;
    State<i64> next;
    Out<TS<i64>> output;

    void start(EvaluationContext* context) const {
        *next = 18;
        context->schedule_at(context->evaluation_time() + timedelta_t{1});
    }
    void evaluate(EvaluationContext* context) const {
        output.set((*next)++);
        if (*next < 20) {
            context->schedule_at(context->evaluation_time() + timedelta_t{1});
        }
    }
};
const auto readings = node<Readings>();
auto temperature() { return readings() + 2; }

int main() {
    const auto result = run_graph(temperature);
    std::cout << std::boolalpha << result.equals({SKIP, 20, 21}) << '\n';
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

Python generators yield an evaluation time and a value. C++ uses start/evaluate
callbacks and the scheduler. Positive future requests add later work; stopping an
instance cancels its remaining scheduled work.

## External producers

Queue handles allow external producers to supply graph updates. The handle's lifetime
and stop behavior must be respected when the owning graph ends.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import Queue, TS, graph, queue, run_graph

@queue
def readings(q: Queue = None) -> TS[int]:
    q.write(18)

@graph
def temperature() -> int:
    return readings() + 2

print(run_graph(temperature))
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
#include <bg/queue_node.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::operators;
using namespace literal;

const auto readings = queue<TS<i64>>(start = [](Queue q) {
    q.write(TimeSeriesLiteral(18));
});

auto temperature() { return readings() + 2; }

int main() {
    const auto result = run_graph(temperature);
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

The queue example supplies a finite input. Realtime integrations additionally select
realtime execution and a run limit or stop mechanism. [Source reference](../../reference/sources.md)
specifies scalar, collection, and batch queue formats.
