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

## Evaluation order

Graph compilation puts nodes in topological order. Every input source is before
the node that receives it. A runtime graph processes scheduled node indices once
in increasing order.

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

`EvaluationContext::add_cycle_callback()` registers one callback. Registration
during stop has no effect because stop code must perform its own cleanup.

A failed or stopped runtime graph discards its pending callbacks. Its stop code
performs the required cleanup.

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

An exception from start or evaluation stops that runtime graph. The engine
invokes stop for every node whose start completed, discards scheduled work and
cleanup callbacks, and propagates the exception. An exception from an inner
graph therefore also stops the outer graph that invoked it.

Output updates are not transactional. Updates made before the exception remain
visible while the stopped graph's storage exists. A stopped runtime graph
cannot restart.
