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
cycle detection but add no input binding or storage. A runtime graph processes
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
- the active `EvaluationProfile`, or `nullptr` when profiling is disabled
- the current runtime graph and node index
- access to the runtime graph's `bg::allocator`
- temporary scratch storage for the current node invocation
- the outer graph's [global state](global-state.md)
- immediate and timed scheduling
- cancellation of timed scheduling
- registration of outer-cycle callbacks

A runtime graph can declare borrowed time-series input slots.
The caller passes a pointer to an array of `RuntimeGraphInput` records to each
`RuntimeGraph::start()`, `evaluate()`, or `stop()` call that needs the inputs.
The array contains at least one record for each slot defined by the compiled
graph. Each record's runtime type must satisfy its compiled binding. Its value,
time-series state, and change must describe that call. The runtime uses the
compiled slot count and does not validate the pointer or records. The caller
keeps the records and their values alive through the call. Slot changes use the
normal input notification and validity paths. A borrowed input cannot be
materialized as a reference because a reference can outlive the call that
supplies the slot. Evaluation contexts do not inherit runtime graph inputs;
ordinary nested graphs pass `nullptr` unless they have their own input records.

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

## Feedback

`feedback<TS_T>(name)` creates a source wiring point. Assign a compatible time
series to the handle once to bind its capture sink. Provider-defined time
series use operations registered with
`TimeSeriesDispatch<TimeSeriesTickOperations>::instance()`:

```cpp
auto fb = bg::feedback<bg::TS<bg::i64>>("loop");
consume(fb);
fb = produce();
```

The provider registers its handlers before graph evaluation. Supported types
and handler requirements are specified in the
[retained-tick format](time-series-types.md#retaining-ticks).

When the input changes in an evaluation cycle, the sink records its final
change state once and schedules the source at the next minimum evaluation
time. The source replays that record as one tick. The source and sink belong to
the same graph. The optional name labels the source and capture sink.

The declared feedback type determines the output guarantee. A delayed `final`
output remains `final`; a delayed output cannot be `const` because its first
tick occurs after graph start.
The feedback type cannot contain references. An input bound through a reference
to a reference-free value series is sampled by normal wiring before capture.

## Simulation

`SimulationEvaluationEngine` starts a runtime graph and evaluates its first
outer cycle. It then advances to each next scheduled time and calls
`evaluate(time)`. It stops the runtime graph when no scheduled evaluation remains.

A later real-time evaluation engine can wait for each scheduled time while
using the same runtime graph interface.

## Evaluation profiling

`EvaluationProfile` records evaluation work when supplied to
`SimulationEvaluationEngine::run()` or to every `RuntimeGraph` start, evaluate,
and stop call in a run. A run uses one profile throughout. Without a profile,
the evaluator does not execute the recording path.

The caller transfers the recording buffer once with `take_recording()` and can
poll it through `EvaluationProfileReader` on another thread. A completed outer
cycle commits its records together. An incomplete outer cycle is rolled back
and disables further recording. The profile writes graph start and stop records
for each runtime graph instance, graph-entry and graph-exit records for nested
graph evaluation calls, outer-cycle boundaries, and a checkpoint after each
scheduled node with valid inputs and an evaluation request, including nodes
with no evaluation function. Nested graph entry and exit records also bracket
pending and resumed evaluations. The outer-cycle boundaries establish and
release the root graph scope; the reader uses that scope and the nested graph
stack to associate node checkpoints with graph instances.

A graph-start record identifies its `CompiledGraph`, parent graph instance, and
owning node. It can also carry a scalar label; map branches use their key as the
label. The `CompiledGraph` objects must remain alive until the reader has
consumed records that refer to them. Labels are typed values stored in the
recording. The reader destroys unread labels when it is destroyed. If the
caller transfers a recording containing labels, it must consume it through a
reader; dropping the raw buffer without a reader does not destroy typed labels.
`recorded_bytes()` counts record payload bytes and excludes buffer alignment
padding and unused block capacity.

On x86, node checkpoints use the timestamp counter. Other targets use
`steady_clock` nanoseconds. The cycle-start record stores evaluation time,
`WallClock::now()`, and a steady-clock reading. The cycle-end record stores the
two clock readings; the reader carries evaluation time from the start record.
A node checkpoint marks completion before output publication. The interval
since the preceding checkpoint includes intervening publication and
scheduling, so it is not an isolated node-function duration. Timestamp-counter
values are comparable across thread migration only when the system synchronizes
its counters across cores. The reader reports the checkpoint clock.

The caller calls `finish()` after stop to publish remaining lifecycle records
and checks `failed()` to detect an incomplete recording. Recording failure or
an aborted outer cycle does not stop graph evaluation; later evaluations use
the plain path. The committed stream may end without stop records. The reader
does not aggregate records or retain performance history.

`EvaluationProfiler` polls the reader on one thread. It keeps directly indexed
cumulative node counters for each active graph. `EvaluationProfilerConfig`
sets independent node and outer-cycle histories. Node history defaults to
one-minute periods retained for 30 minutes. Outer-cycle timing defaults to
one-second periods retained for 24 hours. Both histories keep their current
period in addition to the configured closed periods. Periods align to
the clock epoch. Results use the graph instance ID; successive uses of a
compact-reduce slot contribute to the same graph results. Node period records
are 16 bytes; the current period uses one activity bit per node to capture a
counter baseline only on its first evaluation. On stop, the profiler releases
node counters and retains period records, graph-level cumulative counters, and
the label until expiry.

The outer cycle's start wall-clock time selects both periods. Its records become
visible to the profiler only after that cycle commits. A later committed cycle
closes earlier periods. An idle graph leaves its current period open. The caller
may call `advance_to()` after it knows that no earlier cycle can still commit,
such as after the recording finishes. An empty reader alone does not establish
that condition. Period queries return closed periods. Outer-cycle timing uses
the steady-clock interval between cycle start and end, including all inner graph
work. Each timing record contains cycle count, total duration, minimum, and
maximum in nanoseconds. Empty outer periods are omitted from enumeration;
missing periods represent no completed cycles. Graph-level checkpoint
ticks include nested graph execution; node checkpoint ticks exclude nested graph
execution and include other work between checkpoints in that graph. Checkpoint
ticks use the reader's clock.

Set `GraphRunOptions::recorder` to `profiler.recorder()` and pass the options to
`run_graph` or [`testing::test_graph`](testing.md#test-layout) to record one simulation run.
The helper starts the reader thread, finishes the recording,
and joins the reader before destroying the compiled graph. Query the profiler
after the helper returns. `root_graph_id()` identifies the outer graph, and the
final periods are closed before return. `outer_totals()` and `outer_periods()`
provide cumulative and retained cycle timing. `graph_ids()`,
`node_period_starts()`, and `node_periods()` enumerate retained node results;
`graph()` and `graph_period()` provide graph metadata and counters. `report()`
formats a short outer-cycle summary with recent period timing and UTC period
starts. Stopped graphs no longer have live `node_totals()`. Node results use
compiled node indices. The profiler does not retain compiled
graph metadata after the run.
A profiler recorder accepts one run.

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
