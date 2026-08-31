# Evaluation

Status: working design. Open implementation choices are listed at the end.

## Outer cycle

An outer cycle is one quantum of evaluation and time. Its evaluation time is
constant across the outer graph and all inner graphs. Two outer cycles cannot
have the same evaluation time.

The engine applies an external stop request between outer cycles.

## Evaluation time

`EvaluationTime` is `std::chrono::sys_time<std::chrono::microseconds>`.
`MIN_DT` is reserved before every valid engine time.

## Evaluation order

Graph compilation puts nodes in topological order. Every input source is before
the node that receives it. A runtime graph processes scheduled node indices once
in increasing order.

A node evaluates at most once in an outer cycle. Multiple input updates schedule
one evaluation.

Inner graph execution and reference-bound inputs must preserve the same forward
order.

## Output changes

After a node function returns, the output storage representation enumerates its
changes. The runtime schedules subscribers for the output and each changed item.

## Required input invalidation

When a node changes from all required inputs valid to at least one required
input invalid, the engine applies its invalidation behavior once. Output
invalidation schedules subscribers.

The runtime records whether all required inputs were valid after the last
check. An input validity change marks this result for refresh. Before node
evaluation, the runtime refreshes it when required and uses the result to decide
whether the node can run.

The [node API](node-api.md#required-input-invalidation) defines the declaration
and function interface. The [validity design](time-series-validity.md) defines
the effects on time series and input views.

Evaluation remains blocked while a required input is invalid. An update or
revalidation of an active input schedules evaluation. The same changes on a
passive input schedule nothing.

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
- wall-clock time from the clock supplied to the evaluation engine
- the current runtime graph and node
- the current runtime graph's memory resource
- the outer graph's [global state](global-state.md)
- immediate and future scheduling
- cancellation of future scheduling
- on-demand cycle callback registration

The supplied wall clock allows tests and replay to control wall-clock results.

## Start

`RuntimeGraph::start(time)` starts a runtime graph. It invokes declared start
functions in topological order. The first outer cycle uses the same evaluation
time. A runtime graph can start once.

Start uses the same parameter packing as evaluation and the evaluation time of
the runtime graph's first outer cycle. It can request the context, inputs,
scalars, output, and private state. It can update outputs and schedule future
evaluation.

`schedule_now()` schedules the current node for evaluation in the first cycle.

## Future scheduling

`schedule_at(time)` schedules the current node at a future evaluation time. A
node can have several pending times. Scheduling records due at the same time
coalesce because the node evaluates once in that outer cycle.

`schedule_at` returns a handle. `cancel(handle)` cancels that pending evaluation
if the handle still identifies it.

An aggregate node that uses future scheduling declares:

```cpp
static constexpr bool uses_future_scheduling = true;
```

A graph has future-scheduling state only when a node declares its use. The
runtime graph reports the earliest pending time. `evaluate(time)` schedules
requests due at that time before it processes nodes.

## Simulation

`SimulationEvaluationEngine` starts a runtime graph and evaluates its first
outer cycle. It then advances to each next scheduled time and calls
`evaluate(time)`. It stops the runtime graph when no future evaluation remains.

A later real-time evaluation engine can wait for each scheduled time while
using the same runtime graph interface.

## Cycle callbacks

Cycle callbacks are registered on demand for graph-owned objects, including
node state, time series state, and collection storage. A callback record
identifies the target, callback function, and owning runtime graph.

A beginning-of-cycle callback runs before new input updates. An end-of-cycle
callback runs after all outer and inner evaluation work and before deferred
storage reclamation.

A failed or stopped runtime graph discards its pending cycle callbacks. Its stop
code performs cleanup. [Deferred reclamation](time-series-references.md#outer-cycle-boundary)
keeps required storage valid through the outer cycle.

## Stop

`RuntimeGraph::stop()` stops a runtime graph. It invokes declared stop functions
in reverse topological order. Calling it before start or after stop has no
effect.

Stop uses the same parameter packing as evaluation. It can request the context,
inputs, scalars, output, and private state. It cannot update outputs or schedule
work. Stop code must be `noexcept`.

The engine invokes stop once for each node whose start completed, including
after graph failure.

## Exceptions

An uncaught exception propagates through inner graph invocations. An exception
that reaches the outer graph is a terminal failure.

Wiring can select an exception-catching evaluation entry point for any node.
This choice adds the exception-catching branch and a separate error time series
to that wired node.

The error value contains:

- the error type
- the error message
- a snapshot that identifies the runtime graph instance, its complete nested
  graph path, and the node that threw

The snapshot stores identifiers instead of runtime graph pointers. This is
required because an inner runtime graph can be removed while its error value
remains observable.

Output updates are not transactional. When an inner graph fails, the engine:

1. sets the error output selected by wiring
2. invalidates the inner graph's reference output and unbinds its subscribers
3. discards scheduled work and future scheduling for that runtime graph
4. discards its cycle callbacks
5. invokes stop immediately for its successfully started nodes
6. keeps storage valid through the outer cycle

The failed runtime graph remains stopped. A later event can replace it with a
new runtime graph, which starts in the replacement cycle.

Failure in the outer graph is terminal. The engine runs stop before reporting
the failure to its caller.

## Open implementation choices

1. C++ representation of wall-clock time.
2. Layout of future scheduling handles and records.
3. Storage layout for callback records.
4. C++ types and storage for structured error values and nested graph paths.
5. Execution ordering across runtime graph boundaries and dynamic references.
