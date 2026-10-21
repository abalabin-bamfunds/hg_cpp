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
