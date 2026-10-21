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

## Queue input

At the beginning of each outer cycle, after deferred cleanup, the runtime graph
activates evaluation-time timers due at the cycle's evaluation time and
wall-clock timers due at the current wall-clock time. If any activate, it leaves
the queue untouched for that cycle. Otherwise, it applies committed queue
messages in order before evaluating nodes. It stops at
the committed watermark or the first message that conflicts with the target's
mode. The conflicting message remains pending, and later messages wait behind
it. See [queue messages](time-series-types.md#queue-messages) for application rules.

Each changed queue target is scheduled for ordinary output publication once in
the cycle. Subscribers observe its completed output after draining. No-op
messages do not schedule the target. Resuming a paused graph does not drain
again in the same cycle.

`next_scheduled_time()` returns the earlier of the next evaluation-time timer and
the next minimum evaluation time when committed queue messages remain. A chain of
minimum-tick timers therefore completes before queue reading resumes. Timers
from inner graphs use the same shared scheduler and priority. Stopped
and finalized graphs do not report queue work.

`has_committed_queue_messages()` reports queue readiness independently of timers.
The outer evaluator's `next_scheduled_time()` reports only evaluation-time
timers. An engine accesses that evaluator through `GlobalEvaluationContext`.

An engine can call `bind_queue_wakeup(wakeup)` once before graph startup. Graphs
without queue declarations require no queue storage. The engine owns the
`details::EngineWakeup` and keeps it alive until queue admission closes.
Queue writes notify it after committing a message, including a sentinel frame after
serialization failure, and before leaving write admission. Root `SKIP` requires
no notification. Closure waits for admitted writers before detaching the wakeup.
Graph stop and finalization close admission. The engine operation `close_queue()`
also closes admission and supports sequential repeated calls; it is required to
detach the wakeup when startup fails before the graph starts.

`EngineWakeup` supports one waiting engine thread and concurrent notifications.
Notifications coalesce. After each wait, the engine rechecks queue readiness,
stop state, and timer deadlines. A timed wait can fail spuriously; the engine
checks the clock before treating a deadline as reached. An external stop request
must notify the wakeup too.

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

`GlobalEvaluationContext` holds services shared by the outer graph and its inner
graphs: the wall clock, global state, evaluation profile, scratch arena, and
references to the outer runtime graph and its evaluator. Nested contexts borrow
the same global context, so access to the outer graph and evaluator does not
traverse graph ownership.

Each evaluation engine creates one global context for a run. A direct
`RuntimeGraph::start`, `evaluate`, or `stop` call creates one for that call.
Overloads that take an `EvaluationContext` borrow its global context.
`EvaluationContext` retains the evaluation time, current graph and node, borrowed
graph inputs, scoped bindings, and invocation-local scratch frames. Evaluation
time remains local for direct access during node execution. Child construction
shares services and keeps scratch-frame state local.

The engine constructs `EvaluationContext(global_context, time, runtime_graph,
inputs)` for lifecycle calls and outer cycles. The global context, its services,
the runtime graph, and any borrowed inputs must outlive that context.

The context provides:

- the evaluation time
- a wall-clock observation through `WallClock::now()`
- the active `EvaluationProfile`, or `nullptr` when profiling is disabled
- the current runtime graph and node index
- access to the runtime graph's `bg::allocator`
- temporary scratch storage for the current node invocation
- the outer graph's [global state](global-state.md)
- immediate and timed scheduling, and wall-clock timers
- timer cancellation and fired-handle queries
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
times. `RealtimeEvaluationEngine` and direct `RuntimeGraph::start`, `evaluate`,
and `stop` calls use `SystemWallClock`.

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
The engine overload `schedule_at(node_index, time)` targets another node in the
current runtime graph.

`schedule_wall_clock_at(time)` makes the current node runnable when
`WallClock::now()` reaches the deadline. Readiness depends on wall-clock time.
Overdue wall-clock timers can coalesce in one outer cycle; their old deadlines
do not select a historical evaluation time. An already due deadline becomes
runnable at the next outer-cycle entry.

Both functions return a `ScheduleHandle` scoped to the outer runtime graph and
the node that registered it. `cancel(handle)` removes a pending timer if the
handle still identifies it. Cancelling an activated timer preserves its fired
status for the rest of the cycle.

`timer_fired(handle)` reports whether that node's timer activated in the current
outer cycle. The result remains available through nested evaluation and
pause/resume. The next outer-cycle entry retires fired records before deferred
cleanup. Graph stop retires the graph's records after its stop functions run.

An aggregate node that uses timed scheduling declares:

```cpp
static constexpr bool uses_timed_scheduling = true;
```

Both timer kinds require this declaration. A graph reserves timer ownership
state only when a node declares its use. The outer evaluator owns one shared
timer scheduler.

`RuntimeGraph::next_scheduled_time()` and `next_wall_clock_time()` are outer-graph
engine operations. The former includes [queue input](#queue-input); the latter
reports the next wall-clock deadline separately. Direct `evaluate(time)` also
activates overdue evaluation-time requests before it processes nodes.

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
The feedback output type cannot contain references. `feedback(point)` infers its
reference-free value type and changes a root `const` guarantee to `final`.
Pass the resolved output type explicitly when inference is not suitable.

## Simulation

`SimulationEvaluationEngine` starts a runtime graph and evaluates its first
outer cycle. It then advances to the earliest evaluation-time timer, simulated
wall-clock deadline, or [queue wake](#queue-input), and calls `evaluate(time)`.
An overdue wall-clock deadline advances to the next minimum evaluation time.
It stops the runtime graph when no scheduled evaluation remains.

## Realtime evaluation

`RealtimeEvaluationEngine::run(graph, start_time, stop_token)` waits until the
explicit start time, starts the graph, and evaluates its first outer cycle at
that time. The optional `std::stop_token` defaults to an empty token. The
profiling overload accepts an `EvaluationProfile&` before the token.

For each later cycle, the engine selects the earlier of the next evaluation-time
timer and a cycle for ready queue or wall-clock work. Ready external work uses
the later of the current wall-clock time and the next minimum evaluation time.
An evaluation-time timer therefore establishes a cycle at its declared time;
external work cannot advance evaluation past it. Timers at different evaluation
times run in separate cycles. Timer activation and queue priority follow
[queue input](#queue-input).

When no cycle is ready, the engine waits for queue publication, the next timer
deadline, or a stop request. It rechecks readiness after every wake. An empty
schedule leaves the engine waiting.

A stop request before startup leaves the graph unstarted. Once startup begins,
the engine completes the first outer cycle and observes stop requests between
complete outer cycles. A stop callback only notifies the engine wakeup; the
engine thread stops the graph through its normal lifecycle. Queue admission
closes before the engine destroys its wakeup, including when startup fails.
Signal handling belongs to the application. A signal waiter thread can request
stop through `std::stop_source` from normal thread execution.

## Periodic callbacks

Both evaluation engines accept `PeriodicCallback` from
`<bg/periodic_callback.hpp>`. Its `callback` function runs on the evaluation
thread before startup, between complete outer cycles, and before normal
termination. Periodic calls use its positive `interval`, which defaults to
50 milliseconds and uses `std::chrono::steady_clock`, independently of evaluation
time. Startup and termination calls run regardless of the interval. An empty
function disables the callback.

During continuous evaluation, engines check the callback deadline after each
group of eight complete outer cycles. A due callback can wait for up to seven
additional cycles. Realtime engines also check immediately before waiting and
after waking. Realtime wait deadlines are bounded by the next callback deadline,
including before a future start time.

Calling the function does not evaluate the graph or advance evaluation time.
An exception from the callback stops a started graph through its normal lifecycle
and propagates to the caller. A callback that returns normally lets execution
continue.

Set `GraphRunOptions::periodic_callback` for `run_graph` or `test_graph`. Profiling
uses the same callback. Callback state belongs to the engine run; runtime graphs and
nodes store no callback state. Runs without a callback retain their normal
wait behavior and do not read a clock for periodic callbacks.

## Evaluation profiling

`EvaluationProfile` records evaluation work when supplied to
`SimulationEvaluationEngine::run()`, `RealtimeEvaluationEngine::run()`, or to
every `RuntimeGraph` start, evaluate, and stop call in a run. A run uses one
profile throughout. Without a profile, the evaluator does not execute the
recording path.

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
`run_graph` or [`testing::test_graph`](testing.md#test-layout) to record one run.
Profiling follows the selected [engine and start time](graph-wiring.md#direct-execution).
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

A node definition that registers cycle callbacks declares
`static constexpr bool uses_cycle_callbacks = true`. This is required to reserve
the runtime graph's callback-storage pointer during graph compilation.

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

An outer runtime graph owns one push queue when its nodes declare queue use.
Before invoking stop functions, it closes queue admission and waits for accepted
writes to finish. After stop functions and cleanup callbacks complete, its reader
discards unread messages using each target node's output type. Startup failure
and evaluation failure use the same cleanup order. Finalization also closes and
discards the queue before destroying node state or outputs.

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
