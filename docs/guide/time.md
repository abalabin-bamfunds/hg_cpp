# Work with evaluation time

To timestamp a calculated result, request the evaluation clock. Evaluation time
identifies the event being processed. Realtime execution also has a wall clock
for current physical time.

Prerequisites: [Running graphs](run-graphs.md), [input views](input-views.md).

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import EvaluationClock, graph, node, test_graph

@node
def microsecond(reading: int, clock: EvaluationClock = None) -> int:
    return clock.evaluation_time.microsecond

@graph
def timestamps(reading: int) -> int:
    return microsecond(reading)

print(test_graph(timestamps, [18, None, 19]))
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
#include <bg/testing/test_graph.hpp>
#include <iostream>
#include <bg/evaluation_context.hpp>

using namespace bg;
using namespace bg::testing;
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
    const auto result = test_graph(graph(timestamps), {18, SKIP, 19});
    std::cout << std::boolalpha << result.equals({1, SKIP, 3}) << '\n';
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

Default simulation starts one microsecond after the epoch. `test_graph` places
successive input positions one microsecond apart, so the third position has
time 3 even when the second input position is skipped.

Python injects `EvaluationClock` into a node parameter with `= None`. Its
`evaluation_time` is a timezone-naive local `datetime`. `now` equals evaluation
time in simulation and wall-clock time in realtime. C++ obtains evaluation time
from `EvaluationContext`; `EvaluationTime` uses microsecond resolution.
The C++ aggregate declares input and output fields and receives the context
in `evaluate`. `node<Microsecond>()` makes that definition callable in graph code;
`output.set` publishes its result.

Durations use `datetime.timedelta` in Python and `timedelta_t` in C++. They are
fixed scalar arguments to the timed library operators.

## Update time

An input view's `last_modified_time` in Python or `last_updated()` in C++
identifies its last valid update. It remains unchanged across skipped positions.
A calculation can read an older current value at a later evaluation time.

Simulation jumps to the next scheduled work. Realtime waits until work is due.
Neither mode changes dependencies or the meaning of a current input value.
[Execution reference](../reference/execution.md) defines start times, run limits,
and cancellation. [Custom sources](sources.md) describe timed node callbacks.
