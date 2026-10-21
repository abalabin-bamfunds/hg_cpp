# Run graphs

`run_graph` constructs and runs a complete application graph. It performs sink
effects and does not return recorded graph output. Use `test_graph` for recording.

## Options

| Purpose | Python keyword | C++ `GraphRunOptions` field |
| --- | --- | --- |
| Engine | `engine` | `engine` |
| Start time | `start_time` | `start_time` |
| Absolute exclusive stop boundary | `stop_time` | `stop_time` |
| Duration from resolved start | `run_time` | `run_time` |
| Realtime cancellation | `stop_token` | `stop_token` |
| Service catalogue | `catalogue` | `services` |
| Profiling | `recorder` | `recorder` |
| Shared scalar state | Not exposed | `global_state` |
| Periodic host callback | Python signal checks supplied by runner | `periodic_callback` |

Python calls `run_graph(definition, *args, **graph_arguments, ...options)`.
Graph arguments are ordinary construction-time arguments, not test sequences.
C++ uses `run_graph(function, options, arguments...)` where the overload accepts
options; a wrapper function can bind graph arguments.

## Engines and limits

Simulation is the default. It advances directly to pending evaluation work and
finishes when no work remains. Realtime uses `EvaluationEngine.realtime` in
Python or `EvaluationEngine::realtime` in C++; it waits for queued work and
timers until cancellation or a run limit.

Simulation defaults to epoch plus one microsecond. Realtime defaults to its
current start time. `stop_time` must be later than start. `run_time` must be
positive and fit the supported time range. If both limits are supplied, the
earlier boundary wins. A boundary is exclusive. Timed work must fit the
supported evaluation-time range.

Python times use timezone-naive local `datetime`; durations use `timedelta`.
C++ uses `EvaluationTime` and its duration. Use evaluation time for event
semantics and the wall clock for physical deadlines.

Python cancellation uses `StopSource().get_token()` and `request_stop()`;
C++ uses `std::stop_source` and its token. Simulation ignores a stop token and
uses its normal work/limit termination. Cancellation completes the current
outer cycle before cleanup. Realtime can wait with no pending internal work.

On Python's main thread, the runners process signals approximately every 50 ms,
subject to completion of the current outer cycle. Ctrl+C raises
`KeyboardInterrupt` after cleanup. Background graph threads use explicit stop
requests. Exceptions from graph construction and evaluation propagate after
cleanup; stop callback failures follow [lifecycle rules](state.md).

## Profiling

Create `EvaluationProfiler`, pass its `.recorder()` to one run, and query it after
execution. `report()` formats outer timing. `outer_totals`, `outer_periods`,
`graph_ids`, `graph`, `node_period_starts`, and `node_periods` expose structured
data. Python uses methods with these names; C++ uses the corresponding profiler
API. A recorder records one graph run. Period lengths and retention counts are
configured on the profiler. See [profiling](../how-to/profiling.md).
