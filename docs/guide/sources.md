# Write a timed source

To produce application-specific values at chosen times, define a timed source.
Use [`schedule`](schedule.md) for regular signals. A custom source can terminate
when its own data is exhausted.

Prerequisites: [Evaluation time](time.md), [lifecycle callbacks](lifecycle.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from bg import TS, generator, graph, test_graph

@generator
def readings() -> TS[int]:
    yield timedelta(microseconds=1), 18
    yield timedelta(microseconds=1), 19

@graph
def temperature() -> int:
    return readings() + 2

print(test_graph(temperature))
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
#include <bg/testing/test_graph.hpp>
#include <iostream>
#include <bg/evaluation_context.hpp>

using namespace bg;
using namespace bg::testing;
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
    std::cout << std::boolalpha << (test_graph(temperature).equals({SKIP, 20, 21})) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
builds this example as `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

A Python generator yields `(time, delta)` pairs. A `timedelta` is relative to
the current evaluation time; a `datetime` is absolute. Times must increase
strictly. Exhaustion ends this source's work. BG closes the generator when its
node stops.

C++ declares timed scheduling explicitly and schedules the first evaluation in
`start`. The retained counter controls when scheduling ends. Both sources
publish at start plus one and two microseconds.

For callback-based Python sources, `@source_node` runs once at graph start and
can request `Scheduler` to arrange later calls. `schedule_at` returns a handle;
`cancel` cancels a pending handle and `timer_fired` identifies it during the
current cycle. Clock and scheduler objects expire after the callback; handles
can be stored in state. See [source reference](../reference/sources.md).
