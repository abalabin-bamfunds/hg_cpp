# Selection and validity

These operators decide which values or updates reach downstream calculations. Python
imports them from `bg.operators`; C++ uses `bg::operators` from `<bg/library.hpp>`.
Inputs can include compatible reference connections.

## valid

`valid(value)` reports root validity. It evaluates in the first cycle, accepts initially
invalid inputs, and produces a deduplicated Boolean output. Reference arguments observe
their targets through normal input bindings. Collection root validity is separate from
[item validity](input-views.md#collection-views).

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
Input invalidation propagates while open; TSS and TSD outputs become valid empty
collections rather than root-invalid. The output retains the input's root guarantee up
to `final`. Contained time series are limited to that same guarantee because reopening
can copy unchanged items. A const input becomes final because its first forwarding can
be delayed.

## if_then_else

`if_then_else(condition, true_value, false_value)` selects an input by reference.
Changing the condition exposes the selected input's current value. Updates to that input
remain observable without copying. An invalid condition or selected input makes the
observed result invalid. Updates to the other input and repeated identical condition
values do not publish additional ticks. Selection applies to the whole collection. Both
inputs must have a [common time series type](types.md#guarantees).

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
    std::cout << std::boolalpha << result.equals({10, SKIP, SKIP, 12, SKIP, 14}) << '\n';
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
