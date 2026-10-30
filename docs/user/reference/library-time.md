# Time operators

Timed operators use evaluation time. C++ durations use `timedelta_t`; Python uses
`datetime.timedelta`. Arguments such as a period are fixed during construction.

## last_modified_time

`last_modified_time(value)` publishes the input's latest update time on each
valid input event. The output invalidates with the input and retains its
guarantee. C++ returns `date_time_t`; Python returns a timezone-naive `datetime`.

## elapsed

`elapsed(value)` publishes the duration between consecutive valid input update
times. The first valid event establishes the baseline without publishing.
Input invalidation resets the baseline and invalidates the output; the next
valid event establishes a new baseline. Skipped cycles do not affect the
duration. Ordinary equal-valued ticks are separate events. The output guarantee
retains the input guarantee up to final.

## delay

`delay(value, duration)` publishes each input event at its original evaluation time plus
`duration`. It retains ticks, including invalidation and collection deltas, rather than
sampling the value later. Equal ordinary ticks remain separate events. Zero duration
returns the input; negative duration is a wiring error. Positive delay limits const
guarantees to final, including contained time series. See
[retained events](#retained-events) for supported types and restrictions.

## schedule

`schedule(period, initial_delay=true, max_ticks=...)` publishes ordinary `TS<bool>`
ticks whose value is true. The first tick occurs one period after graph start.
`initial_delay=false` produces the first tick at graph start. `max_ticks` limits the
number of ticks; zero produces none. Without a limit, the source continues until its
graph stops. Period must be positive and `max_ticks` must be nonnegative.

## throttle

`throttle(value, period, delay_first_tick=false)` passes the first event immediately and
starts a period. Events during that period coalesce. At its end, throttle publishes the
latest pending state and starts another period. With no pending event, it stops
scheduling until the input changes again. `delay_first_tick=true` also delays the first
event by one period. Period must be positive.

Throttle applies the same timing to invalidation. An input change at the publication
time contributes to that publication. Collection publication reconciles the complete
current value, so changes that cancel within the period need not produce an output tick.
The output retains input guarantees up to final. Throttle retains timing state, not a
copy of the input value.

## debounce

`debounce(value, period)` publishes the complete latest value after the input has
been unchanged for `period`. Every input event restarts the period, including
equal ordinary ticks, invalidation, collection changes, and reference changes.
An event at the expiry time restarts the period without publishing. Debounce
does not schedule an initial publication. Period must be positive. The output
guarantee is limited to final, including contained time series.

## timeout

`timeout(value, period)` publishes an ordinary `TS[bool]` tick with value true
after a period without input events. Every input event restarts the period,
including equal ordinary ticks, invalidation, collection changes, and reference
changes. An event at expiry restarts the period without publishing. Timeout
does not schedule an initial tick. After its first expiry, the output remains
valid and true; each later expiry publishes another true tick. Period must be
positive.

Timed work uses evaluation time and stops with the owning graph, including inner
graphs. Scheduling outside the supported [evaluation-time range](execution.md) fails.

## batch

`batch(condition, value, delay, buffer_length=...)` collects valid scalar values into
an owning `TS<List<V>>`. Opening publishes all queued values immediately. While open,
the first new value starts the timer; the timer publishes all queued values together.
Repeated true condition ticks do not flush early. Closing cancels the timer and retains
queued values; reopening publishes them immediately. Invalidated values are not
appended. `delay` and `buffer_length` must be positive; overflow raises an error. The
output guarantee is ordinary. For event-by-event retention, see [`gate`](library-selection.md#gate).

## Retained events

Delay retains owning tick data for supported scalar, set, dictionary, bundle, and
fixed-list shapes, including invalidation. Contained references are dereferenced
before capture. Delay rejects sparse TSL values. Initially unbound references are
supported. A retained scalar refers to its owned value at the input event, not
the source's later current value. Throttle retains timing state and reads the
current source at publication, reconciling collection state then.

Gate retains input events using the same ownership and reference rules as delay.
Retained collection deltas require a matching output baseline. Batch copies each
valid scalar value into its output list; later source updates cannot change queued
values.

## resample

`resample(value, period)` composes a periodic `schedule` signal with `sample`.
The first sample occurs one period after graph start. It republishes the current
valid value on each signal; source changes alone do not publish a sample.
The period must be positive. A finite run limit is required when no other stop
condition ends the periodic source.

The [sampling example](../guide/graphs/updates.md#signal-driven-publication) shows the
publication rule; the [periodic example](../guide/execution/time.md#periodic-signals)
shows period and run-limit syntax.


<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from bg import graph, run_graph
from bg.operators import resample

@graph
def snapshots(value: int) -> int:
    return resample(value, timedelta(microseconds=2))

result = run_graph(snapshots, [10, None, None, 20], run_time=timedelta(microseconds=5))
print(result)
```

Output:

```text
[None, None, 10, None, 20]
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

auto snapshots(wp<TS<i64>> value) {
    return resample(value, timedelta_t{2});
}

int main() {
    const auto result = run_graph(snapshots, GraphRunOptions{.run_time = EvaluationTime::duration{5}}, {10, SKIP, SKIP, 20});
    std::cout << result << '\n';
}
```

Output:

```text
[None, None, 10, None, 20]
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
