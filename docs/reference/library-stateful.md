# Calculations over successive updates

Use these operators for a history over updates to one value. Each runtime node retains
its own state. Repeated calls that BG reuses during graph construction share that node;
separate runtime graph instances have separate state.

## count

`count(value)` publishes `1` on the first valid input tick and increments on each later
valid tick. Invalidation invalidates the output and resets the count; revalidation
starts at `1`. It counts root ticks, not collection items, and propagates the observed
input's root guarantee. Incrementing uses the [`i64` wrapping
policy](library-numeric.md#arithmetic).

## lag

`lag(value)` accepts `TS<T>` and publishes the preceding input event whenever the input
changes. It retains an owning copy of the scalar value. Invalidation is also an event
and is delayed by one input event:

```text
input:   10   20   invalid   30        40
output:  —    10   20        invalid   30
```

Neither operator schedules on start. Skipped cycles do not advance either operation.
Ordinary equal-valued ticks remain separate events. `lag` produces an ordinary output;
its first event produces no output, and its final retained event remains unpublished
unless another input event arrives.

## sums, mins, maxs, means

`sums(value)`, `mins(value)`, `maxs(value)`, and `means(value)` calculate over
successive ticks of one `TS<i64>` or `TS<double>`. `sums`, `mins`, and `maxs` retain the
input scalar type. `means` returns `double` and accumulates in `double`, independently
of the integer sum's wrapping policy.

Each valid input tick contributes, including equal ordinary ticks. Skipped cycles do not
advance the calculation. Invalidation clears history and invalidates the output;
revalidation starts a new history. No operator schedules on start. `mins` and `maxs`
publish the first value and then publish only when a comparison establishes a new
extremum.

`mins` and `maxs` use ordinary floating-point comparisons. An initial NaN remains the
result until history is cleared; a later NaN produces no tick. NaN is a valid scalar
value, not time series invalidity.

Without reset, `sums` and `means` propagate the observed input guarantee. `mins` and
`maxs` provide at least `deduplicated` and retain `final` or `const` inputs. With reset,
`sums` and `means` produce ordinary outputs, while `mins` and `maxs` produce
deduplicated outputs. Reset can invalidate these outputs independently of the value
input's guarantee.

Each operator accepts an optional `reset` time series argument. Any valid tick on that
input clears history, regardless of its value. A simultaneous valid `value` tick starts
the new history. Reset alone invalidates the output rather than sampling an unchanged
input. Invalidating `reset` does not clear history or contribute another value.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg.operators import sums

total = sums(value)
restarted_total = sums(value, reset=reset)
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
using namespace bg::literals;

auto total = sums(value);
auto restarted_total = sums(value, "reset"_n = reset);
```
:::
::::

## diff

`diff(value)` subtracts the previous valid value from the current value. It accepts
integer and floating-point time series and returns the same scalar type with an
ordinary guarantee. The first value after start or invalidation produces no result.
Skipped positions do not advance history.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import diff

@graph
def change(value: int) -> int:
    return diff(value)

print(run_graph(change, [10, 13, None, 12]))
```

Output:

```text
[None, 3, None, -1]
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

auto change(wp<TS<i64>> value) {
    return diff(value);
}

int main() {
    const auto result = run_graph(change, {10, 13, SKIP, 12});
    std::cout << std::boolalpha << result.equals({SKIP, 3, SKIP, -1}) << '\n';
}
```

Output:

```text
true
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

## ewma

`ewma(value, alpha, min_periods=0)` maintains an exponentially weighted moving average
of floating-point values. `alpha` is a fixed finite value in `[0, 1]`. The first
valid value seeds the average; each later value applies
`average += alpha * (value - average)`.

A nonnegative `min_periods` suppresses that many initial valid ticks. Invalidation
clears the average and warmup count and invalidates the result. Skipped positions
preserve state. Zero warmup retains the input guarantee; positive warmup limits const
to final.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import ewma

@graph
def smoothed(value: float) -> float:
    return ewma(value, 0.5)

print(run_graph(smoothed, [10.0, 14.0, 16.0]))
```

Output:

```text
[10.0, 12.0, 14.0]
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

auto smoothed(wp<TS<double>> value) {
    return ewma(value, 0.5);
}

int main() {
    const auto result = run_graph(smoothed, {10.0, 14.0, 16.0});
    std::cout << std::boolalpha << result.equals({10.0, 12.0, 14.0}) << '\n';
}
```

Output:

```text
true
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
