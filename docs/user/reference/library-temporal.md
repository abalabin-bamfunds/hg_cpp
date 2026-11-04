# Timestamps and durations

When a calculation needs a deadline, an elapsed amount, or a calendar field, use
timestamp and duration values with ordinary graph operators. They update with their
inputs just like numbers. To schedule graph evaluation or delay events, use the
[time operators](library-time.md) instead.

Python uses `datetime.datetime` and `datetime.timedelta`. C++ uses `date_time_t`
(a microsecond-resolution `std::chrono::sys_time`) and `timedelta_t`
(`std::chrono::microseconds`). Python datetimes represent timezone-naive UTC values.

## Timestamp arithmetic

To calculate how long remains before a deadline, subtract the current timestamp
from the deadline. Adding or subtracting a duration shifts a timestamp; subtracting
two timestamps produces a duration:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import datetime, timedelta
from hgraph import graph, run_graph

@graph
def seconds_remaining(now: datetime) -> float:
    return (datetime(2026, 1, 1, 12) - now) / timedelta(seconds=1)

print(run_graph(seconds_remaining, [
    datetime(2026, 1, 1, 11, 59, 50),
    datetime(2026, 1, 1, 12, 0, 5),
]))
```

Output:

```text
[10.0, -5.0]
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

#include <chrono>

using namespace hg;
using namespace hg::operators;
using namespace std::chrono;
using namespace std::chrono_literals;

int main() {
    const auto deadline = time_point_cast<timedelta_t>(sys_days{2026y/January/1} + 12h);
    std::cout << run_graph([deadline](auto now) {
        return (deadline - now) / timedelta_t{1s};
    }, {deadline - 10s, deadline + 5s}) << '\n';
}
```

Output:

```text
[10, -5]
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

Timestamp and duration addition and subtraction wrap at the underlying integer
width. Comparisons require matching temporal types. Duration negation and `abs`
are supported; the minimum representable duration remains unchanged by either.

## Scaling and comparing durations

To adjust a timeout or express elapsed time in a chosen unit, multiply or divide a
duration by a number, or divide it by another duration. A duration ratio returns
`double`; `floordiv` returns the number of complete units, rounded down:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from hgraph import graph, run_graph, symbol
from hgraph.operators import floordiv, getattr

@graph
def half_seconds(duration: timedelta) -> float:
    return getattr(duration / 2, symbol("total_seconds"))

@graph
def complete_minutes(duration: timedelta) -> int:
    return floordiv(duration, timedelta(minutes=1))

values = [timedelta(seconds=90), timedelta(seconds=150)]
print(run_graph(half_seconds, values))
print(run_graph(complete_minutes, values))
```

Output:

```text
[45.0, 75.0]
[1, 2]
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

#include <chrono>

using namespace hg;
using namespace hg::operators;
using namespace std::chrono_literals;

int main() {
    std::cout << run_graph([](auto duration) {
        return operators::getattr(duration / 2, symbol{"total_seconds"});
    }, {timedelta_t{90s}, timedelta_t{150s}}) << '\n';
    std::cout << run_graph([](auto duration) {
        return floordiv(duration, timedelta_t{1min});
    }, {timedelta_t{90s}, timedelta_t{150s}}) << '\n';
}
```

Output:

```text
[45, 75]
[1, 2]
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

Multiplication accepts an integer or double on either side. Integer multiplication
wraps at `i64` width. Division by an integer, and multiplication or division by a
double, round microseconds to nearest with ties to even. Division rejects zero or
non-finite divisors with `domain_error`. Non-finite or out-of-range scaled results
raise `overflow_error`.

Floating scaling uses native `long double` intermediates. Halfway results can vary
on platforms where `long double` has no more precision than `double`; for example,
`5us * 0.1` can round to `0us`. Exact parity with Python's scalar `timedelta`
multiplication is not guaranteed.

## Calendar and duration fields

If a calculation depends on a date component or needs a duration in seconds,
`getattr(value, symbol("field"))` extracts that field as a changing value. The field
name is fixed during graph construction:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import datetime, timedelta
from hgraph import graph, run_graph, symbol
from hgraph.operators import getattr

@graph
def weekday(value: datetime) -> int:
    return getattr(value, symbol("weekday"))

@graph
def day_component(value: timedelta) -> int:
    return getattr(value, symbol("days"))

print(run_graph(weekday, [datetime(2026, 1, 5), datetime(2026, 1, 11)]))
print(run_graph(day_component, [timedelta(microseconds=-1), timedelta(days=2)]))
```

Output:

```text
[0, 6]
[-1, 2]
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

#include <chrono>

using namespace hg;
using namespace hg::operators;
using namespace std::chrono;
using namespace std::chrono_literals;

int main() {
    std::cout << run_graph([](auto value) {
        return operators::getattr(value, symbol{"weekday"});
    }, {time_point_cast<timedelta_t>(sys_days{2026y/January/5}),
        time_point_cast<timedelta_t>(sys_days{2026y/January/11})}) << '\n';
    std::cout << run_graph([](auto value) {
        return operators::getattr(value, symbol{"days"});
    }, {timedelta_t{-1us}, timedelta_t{48h}}) << '\n';
}
```

Output:

```text
[0, 6]
[-1, 2]
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

| Value | Fields | Result |
| --- | --- | --- |
| Timestamp | `year`, `month`, `day`, `hour`, `minute`, `second`, `microsecond` | `i64` component |
| Timestamp | `weekday`, `isoweekday` | `i64`; Monday is 0 or 1 respectively |
| Duration | `days`, `seconds`, `microseconds` | Normalized `i64` component |
| Duration | `total_seconds` | Complete duration as `double` seconds |

Duration components match Python's normalization: `-1us` has `days=-1`,
`seconds=86399`, and `microseconds=999999`. `seconds` is the remainder within a day;
`total_seconds` includes the days and fractional seconds.

Calendar fields raise `out_of_range` when the timestamp's year is outside
`std::chrono::year_month_day`'s range. Time-of-day and weekday fields support the
full `date_time_t` representation. An unsupported field is a wiring error.
