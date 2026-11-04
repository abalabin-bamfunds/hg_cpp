# Graph execution

To evaluate a graph with test inputs or run it as an application, use `run_graph`.
It constructs the graph, supplies arguments, and optionally captures output, with
simulation for scheduled work and realtime execution for live inputs.

## Calls

Pass the graph definition followed by its arguments; add runner options when the run
needs a particular engine, time limit, or service catalogue:

| Language | Form |
| --- | --- |
| Python | `run_graph(definition, *args, capture_output=True, **options_and_graph_arguments)` |
| C++ | `run_graph(definition, arguments...)` |
| C++ with options | `run_graph(definition, GraphRunOptions{...}, arguments...)` |

Python requires a decorated graph. C++ accepts a graph function, graph template, or
supported callable. Arguments bind to the receiving signature. Fixed scalar arguments
remain fixed; input sequences supply successive updates. `ticks` explicitly identifies
a sequence when argument shape would otherwise be ambiguous.

### C++ inferred inputs

For a calculation that can accept the supplied scalar types, use an inline lambda
with `auto` parameters. Each parameter receives a graph connection, and `run_graph`
infers its type from the input values:

```cpp
run_graph([](auto reading) { return reading + 2; }, {18, SKIP, 19});
```

A scalar argument supplies one initial value; a sequence supplies ticks. Capture
fixed configuration in the lambda or pass it to a helper function from the body.
The [basic syntax example](../guide/getting-started.md#changing-inputs) shows a
complete program.

Braced sequences support up to four inputs in this form. For more inputs, or a
call that combines sequences with bare scalar arguments, use named C++ ranges or
explicit input descriptors so C++ can deduce each argument's type.

Literal streams need a value that identifies their scalar type. `SKIP` and
`INVALID` do not identify a type, and values of different scalar types cannot be
combined in one inferred stream. A typed C++ range supplies its element type even
when empty. Ranges of `Scalar<>` or `ScalarValue<>` use the values' runtime types
and need a value to identify a type. Use
`input<TS<T>>({...})` when neither the values nor the range type identify a type.
Collection literals require an explicit type to identify their shape.
Concrete graph declarations continue to determine their own argument interpretation.

## Arguments

To reproduce independently changing sources, supply an input sequence for each one.
The sequences can have different lengths, with each position one microsecond after
the previous position. An exhausted sequence supplies no further updates, and values
remain current until replaced or invalidated. Raw C++ brace calls support one through
four input sequences; descriptors support additional inputs and explicit input types.

A scalar collection can be one fixed argument or a sequence of collection values.
Use `ticks` when the collection's shape is ambiguous. An empty collection matching the
parameter type is one initial empty value; `ticks({})` supplies no ticks in C++.

In C++, inline literal descriptors borrow their tick storage and scalar sources
through the synchronous `run_graph` call. The `input()` overloads and brace-based
`ticks()` return a const reference to the temporary descriptor argument. Use that
reference inline or copy it into a value; do not retain the returned reference:

```cpp
run_graph(graph, input<TS<i64>>({1, 2}));
auto saved = input<TS<i64>>({1, 2}); // copies and owns a snapshot
```

Copying a borrowed `GraphRunLiteralInput` snapshots recursive sequences and scalar
payloads. Copies of an owned descriptor share its immutable snapshot; moving transfers
it. Range-based `ticks(range)` borrows lvalue ranges and owns rvalue ranges through
the returned view. See [input literals](literals.md) and [result access](testing.md).

## Options

Use runner options to choose how the graph executes and which application resources
it receives. Python passes these as keywords; C++ collects them in `GraphRunOptions`:

| Purpose | Python keyword | C++ `GraphRunOptions` field |
| --- | --- | --- |
| Capture output | `capture_output` | `capture_output` |
| Engine | `engine` | `engine` |
| Start time | `start_time` | `start_time` |
| Exclusive stop boundary | `stop_time` | `stop_time` |
| Duration from resolved start | `run_time` | `run_time` |
| Realtime cancellation | `stop_token` | `stop_token` |
| Service catalogue | `catalogue` | `services` |
| Profiling | `recorder` | `recorder` |
| Shared scalar state | Application-owned storage | `global_state` |
| Periodic host callback | Runner supplies Python signal checks | `periodic_callback` |

## Engines and limits

For recorded inputs or deterministic tests, use simulation to advance directly to
pending work and finish when no work remains. For live producers, choose
`EvaluationEngine.realtime` in Python or `EvaluationEngine::realtime` in C++ so the
runner waits for queued work or timers.

Simulation defaults to epoch plus one microsecond. Realtime defaults to the current
start time. `stop_time` must be later than start; `run_time` must be positive. If both
limits are supplied, the earlier boundary wins. Work at the boundary is excluded.
Scheduled work must fit the supported evaluation-time range.

Python uses timezone-naive UTC `datetime` for timestamps and `timedelta` for durations.
C++ uses `EvaluationTime` and its duration. Evaluation time describes graph events;
wall-clock time describes physical time.

To stop a live run from application code, pass a stop token and request cancellation.
Python uses `StopSource().get_token()` and `request_stop()`; C++ uses
`std::stop_source`. Simulation ignores stop tokens, while realtime cancellation
completes the current outer cycle before cleanup.

On Python's main thread, the runner processes signals approximately every 50 ms,
subject to completion of the current outer cycle. Ctrl+C raises `KeyboardInterrupt`
after cleanup. Background graph threads use explicit stop requests.
Construction and evaluation exceptions propagate after cleanup.

## Results and profiling

To inspect publications after a run, keep output capture enabled, which is the
default. Python returns `GraphRunResult`, or `None` for an outputless run or disabled
capture. C++ returns a `GraphRunResult<T>`; disabled capture leaves its event storage
empty. For a generic lambda, `run_graph` returns `GraphRunResult<>`, whose captured
value type is determined at runtime. Capture dereferences a root reference.
Collections containing reference items require capture to be disabled.

[Result access](testing.md) specifies sparse events and dense positions.
[Profiling](../how-to/profiling.md) specifies recorder setup, reported measurements,
and interpretation. A profiler recorder belongs to one run.
