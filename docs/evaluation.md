# Evaluation

## Evaluation model

The outer graph, inner graphs, reference bindings, and runtime-selected
dependencies form one logical dependency graph. The graph remains directed and
acyclic. Nesting and ownership do not change evaluation semantics.

Every logical time series has one authoritative state and change record.
References, boundaries, and representations expose that state. They do not
reproduce or independently propagate it.

An outer cycle evaluates the logical dependency graph at one evaluation time.
Each node observes the completed current-cycle state of its predecessors and
completes at most one evaluation. The cycle ends only after all work for its
evaluation time is complete.

Storage lifetime does not change semantics. Invalidation and removal take
effect in the current outer cycle. Retention can delay only storage reclamation.

## Outer cycle

An outer cycle is one quantum of evaluation and time. Its evaluation time is
constant across the outer graph and all inner graphs. Two outer cycles cannot
have the same evaluation time.

The engine applies an external stop request between outer cycles.

## Evaluation time

`EvaluationTime` is `std::chrono::sys_time<std::chrono::microseconds>`.
`MIN_DT` is reserved before every valid engine time.
Time series store a positive evaluation-time count in 62 bits. The engine
rejects a start or outer-cycle time outside `[MIN_DT + 1 microsecond, 2^62 - 1
microseconds]`.

## Evaluation order

Graph compilation emits each node's input dependencies in signature order before
that node while scanning nodes in wiring order. It rejects dependency cycles.
`GraphWiring` may also add a no-payload ordering constraint that places a node
after every direct consumer of a wiring point. These constraints participate in
cycle detection but add no runtime input or storage. A runtime graph processes
scheduled node indices once in increasing order.

Multiple input updates coalesce into one completed node evaluation in an outer
cycle.

A scheduled node without an evaluation function still processes input validity, invalidation, and output publication.

Inner graph execution and reference-bound inputs must preserve the same forward
order.

## Pending evaluation

A node evaluation function can return pending when another runtime graph must
run before that node can finish. A `bool evaluate()` returns `true` for complete
and `false` for pending. A `void evaluate()` always completes. The runtime graph
retains a pending node and returns pending to its owner. Each intervening inner
graph owner returns pending without interpreting it. The owner that can satisfy
the dependency runs the required graph, then resumes the paused graph at the
same node.

A node output is published only after its runtime function returns complete.
The complete pause and resume sequence runs in one outer cycle. Pending
evaluation that reaches the outer graph is an error.

## Output changes

After a node function returns, the output runtime type enumerates its
changes. The runtime schedules subscribers for the output and each changed item.

## Required input invalidation

When a node changes from satisfying every input validity constraint to failing
at least one constraint, the engine applies its invalidation behavior once. Output
invalidation schedules subscribers.

The runtime records whether every input validity constraint passed after the last
check. A required input's invalidation or revalidation refreshes this result and
can apply the node's invalidation behavior. Before every scheduled evaluation
attempt, the runtime checks `all_valid` inputs again, even if the cached result
passed. It uses the refreshed result to decide whether the node can run.

The `all_valid` check inspects immediate contained time series. Changes to an
item do not trigger this scan on their own. An active input schedules evaluation
on updates, so the scan occurs before that evaluation. A passive input does not
schedule evaluation on updates; its contained-item validity is checked when
another event schedules the node, such as a timer or another input. If an item
change also invalidates or revalidates the input root, the required-input
validity notification refreshes the full validity conjunction immediately.

The [node API](node-api.md#required-input-invalidation) defines the declaration
and function interface. The [validity design](time-series-validity.md) defines
the effects on time series and input views.

Evaluation remains blocked while an input validity constraint fails. An update
or revalidation of an active input schedules evaluation. The same changes on a
passive input do not schedule evaluation.

An input with `dynamic_activity` can change this scheduling behavior during a
node invocation. Its current activity applies only to scheduling caused by that
input. Input validity checks and input callbacks continue regardless
of its current activity.

## Evaluation context

`EvaluationContext` provides services during one node invocation. A function
requests it explicitly as its first parameter:

```cpp
[](bg::EvaluationContext* context, float value) {
    // ...
}
```

The engine injects the context independently of wiring.

The context provides:

- the evaluation time
- a wall-clock observation through `WallClock::now()`
- the current runtime graph and node index
- access to the runtime graph's `bg::allocator`
- temporary scratch storage for the current node invocation
- the outer graph's [global state](global-state.md)
- immediate and timed scheduling
- cancellation of timed scheduling
- registration of outer-cycle callbacks

An inner runtime graph receives a child `EvaluationContext` derived from the
context used to evaluate its owning node. This preserves evaluation services
and internal sections through any number of nested graph owners.

`SimulationEvaluationEngine::run()` owns one simulated wall clock for the run.
The engine sets it to the start time before graph startup, then updates it once
at the start of each later outer evaluation cycle. All contexts and nested
graphs in a cycle share it, so `WallClock::now()` returns that cycle's
evaluation time. Start and stop functions observe the start and final evaluation
times. Direct `RuntimeGraph::start`, `evaluate`, and `stop` calls use
`SystemWallClock`. A real-time engine can provide another `WallClock`
implementation. Timed scheduling uses evaluation time.

## Start

`RuntimeGraph::start(time)` starts a runtime graph. It invokes declared start
functions in topological order. The first outer cycle uses the same evaluation
time. A runtime graph can start once.

Start uses the same parameter packing as evaluation and the evaluation time of
the runtime graph's first outer cycle. It can request the context, inputs,
scalars, output, and private state. It can update outputs and schedule later
evaluation.

Compiled scalar constants become valid at start. Their input notifications run
after start functions so a value-to-reference node has established its runtime
reference identity before it forwards the notification.

`schedule_now()` schedules the current node for evaluation in the first cycle.

## Timed scheduling

`schedule_at(time)` schedules the current node for a later evaluation time. A
node can have several pending times. Scheduling records due at the same time
coalesce because the node evaluates once in that outer cycle.

`schedule_at` returns a handle. `cancel(handle)` cancels that pending evaluation
if the handle still identifies it.

An aggregate node that uses timed scheduling declares:

```cpp
static constexpr bool uses_timed_scheduling = true;
```

A graph has timed scheduling state only when a node declares its use. The
runtime graph reports the earliest pending time. `evaluate(time)` schedules
requests due at that time before it processes nodes.

## Simulation

`SimulationEvaluationEngine` starts a runtime graph and evaluates its first
outer cycle. It then advances to each next scheduled time and calls
`evaluate(time)`. It stops the runtime graph when no scheduled evaluation remains.

A later real-time evaluation engine can wait for each scheduled time while
using the same runtime graph interface.

## Deferred cleanup

Runtime subsystems register cleanup callbacks on demand. The outer graph runs
them in reverse registration order at the beginning of its next cycle, before
it applies new input updates. This keeps removed storage available for the
complete cycle in which it was removed and releases downstream objects before
objects that they can reference.

`EvaluationContext::add_cycle_callback()` registers one callback. The callback
runs at the beginning of the next outer cycle. Registration during stop has no
effect.

A failed or stopped runtime graph discards its pending callbacks. A callback
registered with `call_on_stop = true` also runs when its owning graph stops
or is finalized. The same function runs in both cases, so callers opt in only
when that function is valid cleanup during stop. Other callbacks keep the
discard behavior.

## Stop

`RuntimeGraph::stop()` stops a runtime graph. It invokes declared stop functions
in reverse topological order. Calling it before start or after stop has no
effect.

Stop uses the same parameter packing as evaluation. It can request the context,
inputs, scalars, output, and private state. It cannot update outputs or schedule
work. Stop code must be `noexcept`.

The engine invokes stop once for each node whose start completed, including
after graph failure.

State-destruction functions run before runtime graph storage is destroyed.
Nested graph owners pass their evaluation context when finalizing inner graphs.
The outer graph creates a context with `SystemWallClock` when it is destroyed.

## Exceptions

An exception from start or evaluation stops that runtime graph. The engine
invokes stop for every node whose start completed, runs cleanup callbacks
registered for stop, discards the other pending callbacks and scheduled work,
and propagates the exception. An exception from an inner graph therefore also
stops the outer graph that invoked it.

Output updates are not transactional. Updates made before the exception remain
visible while the stopped graph's storage exists. A stopped runtime graph
cannot restart.
