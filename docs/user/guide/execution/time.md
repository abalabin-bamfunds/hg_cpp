# Time patterns

If a result should refresh periodically, arrive after a delay, or publish at a limited
rate, use time operators to schedule that work. Durations use `datetime.timedelta`
in Python and `timedelta_t` in C++.

## Periodic signals

To trigger work at a regular interval, use `schedule` with a positive period.
Connect its signal to the work it should trigger, and use `max_ticks` when the
source should stop after a fixed number of signals.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from hgraph import graph, run_graph
from hgraph.operators import count, schedule

@graph
def requests() -> int:
    return count(schedule(timedelta(microseconds=1), max_ticks=3))

print(run_graph(requests))
```

Output:

```text
[None, 1, 2, 3]
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
using namespace hg::literals;
using namespace hg::operators;
using namespace literal;

auto requests() {
    return count(schedule(timedelta_t{1}, "max_ticks"_n = 3));
}

int main() {
    const auto result = run_graph(requests);
    std::cout << result << '\n';
}
```

Output:

```text
[None, 1, 2, 3]
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

The default first signal occurs one period after graph start. `initial_delay=False`
includes a signal at graph start. C++ named arguments use `_n` from `hg::literals`.
Connect the signal to `sample` for periodic snapshots.
[`resample`](../../reference/library-time.md#resample) combines those operations; its
example uses a run limit to stop the periodic source.

## Delay

If each input event should appear a fixed duration later, use `delay`. It schedules
a later publication for each event and preserves their order, including repeated
equal values.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from hgraph import graph, run_graph
from hgraph.operators import delay

@graph
def delayed(reading: int) -> int:
    return delay(reading, timedelta(microseconds=2))

print(run_graph(delayed, [18, 19, 19]))
```

Output:

```text
[None, None, 18, 19, 19]
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

auto delayed(wp<TS<i64>> reading) {
    return delay(reading, timedelta_t{2});
}

int main() {
    const auto result = run_graph(delayed, {18, 19, 19});
    std::cout << result << '\n';
}
```

Output:

```text
[None, None, 18, 19, 19]
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

The captured result includes scheduled output after the input sequence ends.
Delay retains pending events; it is different from `lag`, which waits for the next
input event to publish the preceding event.

## Throttle

To limit how often downstream work runs while keeping the latest input, use
`throttle`. Several updates within one interval can become one later publication
containing the latest pending value.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from hgraph import graph, run_graph
from hgraph.operators import throttle

@graph
def reported(reading: int) -> int:
    return throttle(reading, timedelta(microseconds=2))

print(run_graph(reported, [18, 19, 20, 21]))
```

Output:

```text
[18, None, 20, None, 21]
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

auto reported(wp<TS<i64>> reading) {
    return throttle(reading, timedelta_t{2});
}

int main() {
    const auto result = run_graph(reported, {18, 19, 20, 21});
    std::cout << result << '\n';
}
```

Output:

```text
[18, None, 20, None, 21]
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

The [time reference](../../reference/library-time.md) specifies interval boundaries,
leading publication, invalidation, and pending work.
