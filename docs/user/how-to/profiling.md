# Profiling

If a graph does more work than expected, attach an `EvaluationProfiler` recorder to
measure the run. Its outer-cycle durations, per-node evaluation counts, and checkpoint
ticks help identify where the work occurs; each recorder belongs to one graph run.

## Finite run

To check how input events translate into evaluations, this finite run includes three
reading updates, a repeated value, and a skipped position. The gap creates no evaluation
work, while a one-microsecond node period keeps the separate input times visible in
the recorded node history.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from hgraph import EvaluationProfiler, graph, run_graph

@graph
def temperature(reading: int) -> int:
    return reading + 2

profiler = EvaluationProfiler(node_period=timedelta(microseconds=1))
result = run_graph(temperature, [18, 18, None, 19], recorder=profiler.recorder())
print(result)
print(profiler.report(0), end="")

graph_id = profiler.root_graph_id()
periods = profiler.node_period_starts()
for period in periods:
    nodes = profiler.node_periods(graph_id, period)
    for node in nodes:
        print(f"node {node['node_index']}: {node['evaluations']} evaluation(s)")
```

Output (ellipsis):

```text
[20, 20, None, 21]
Outer graph: 3 cycles, average ... ns, min ... ns, max ... ns
node 1: 1 evaluation(s)
node 2: 1 evaluation(s)
node 0: 1 evaluation(s)
node 1: 1 evaluation(s)
node 2: 1 evaluation(s)
node 0: 1 evaluation(s)
node 1: 1 evaluation(s)
node 2: 1 evaluation(s)
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <hg/evaluation_profiler.hpp>
#include <chrono>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto temperature(wp<TS<i64>> reading) {
    return reading + 2;
}

int main() {
    auto profiler = EvaluationProfiler{{.node_period = std::chrono::microseconds{1}}};
    const auto result = run_graph(
        temperature, GraphRunOptions{.recorder = profiler.recorder()}, {18, 18, SKIP, 19});
    std::cout << result << '\n';
    std::cout << profiler.report(0);

    const auto graph_id = *profiler.root_graph_id();
    for (const auto period : profiler.node_period_starts()) {
        const auto nodes = profiler.node_periods(graph_id, period);
        for (const auto& node : nodes) {
            std::cout << "node " << node.node_index << ": " << node.totals.evaluations << " evaluation(s)\n";
        }
    }
}
```

Output (ellipsis):

```text
[20, 20, None, 21]
Outer graph: 3 cycles, average ... ns, min ... ns, max ... ns
node 1: 1 evaluation(s)
node 2: 1 evaluation(s)
node 0: 1 evaluation(s)
node 1: 1 evaluation(s)
node 2: 1 evaluation(s)
node 0: 1 evaluation(s)
node 1: 1 evaluation(s)
node 2: 1 evaluation(s)
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project)
builds this example as `main.cpp`, linked to `hg::library`.
:::
::::
:::::
::::::

The numeric timing fields vary by run; `...` stands for those measurements.
`report(0)` prints cumulative timing without recent-period rows. `report()`
also lists recent outer periods.

## Timing report

To compare cycle costs or find occasional expensive cycles, inspect the outer timing
report. It measures computation within each cycle rather than waits between inputs:

| Field | Meaning | Useful for |
| --- | --- | --- |
| Cycles | Completed outer cycles | Checking the amount of input-driven work |
| Average | Total elapsed computation time divided by cycle count | Comparing typical cycle cost for the same workload |
| Min | Shortest cycle duration | Identifying variation against the maximum |
| Max | Longest cycle duration | Finding occasional expensive cycles |

For programmatic access, `outer_totals()` returns `cycles`, `total_nanoseconds`,
`min_nanoseconds`, and `max_nanoseconds` as a Python dictionary or C++ struct.
An outer cycle includes work in its inner graphs.

Simulation advances event time without wall-clock waits. The one-microsecond
input spacing is an evaluation-time interval, not a promised computation time.
Realtime uses the same cycle measurements under its delivery schedule.

## Evaluation counts

To check whether a filter, sample, or deduplication suppresses the intended work,
compare per-node evaluation counts from `node_periods(graph_id, period)`. It returns
rows containing `node_index`, `evaluations`, and `checkpoint_ticks`. In Python these
are dictionary entries; in C++ the counts are in each row's `totals` field.
The example prints per-node counts for each closed period. Node 1,
the addition in this compiled graph, evaluates once for each reading update,
including the repeated `18`. Test inputs and output recording also contribute
nodes: these indices are specific to this example.

Checkpoint ticks measure intervals between node checkpoints. They include
intervening publication and scheduling work, and exclude nested graph execution
from the owning node's interval. On x86 they use timestamp-counter ticks;
other targets use steady-clock nanoseconds. Compare them as work indicators
under the same clock and configuration; they are not isolated callback times.

To locate work inside a mapped branch or another inner graph, use `graph_ids()` and
`graph(id)` to identify each instance's parent, owner, and label. Map branches use
their key as the label. Node indices are local to
a compiled graph; do not compare an index across changed graph constructions.
C++ compiled-graph metadata is available only while the graph is active.

## History retention

If you need recent performance history, choose period lengths and retention before
the run. Period queries return closed periods, and the runner finishes recording
before returning, so the finite run above can query its final periods. Node history
defaults to one-minute periods with 30 retained closed periods; outer timing
defaults to one-second periods with 86,400 retained closed periods. A current
period is retained in addition to those limits.

After a graph stops, query retained node periods; cumulative active-node counters
have been released.
Keep the workload, engine, profiling configuration, and machine consistent when
comparing performance. See [execution reference](../reference/execution.md#results-and-profiling).
