# Realtime cycles preserve exact timer boundaries

Status: accepted

## Context

Queue input and wall-clock timers can become ready while evaluation-time timers
remain pending. Evaluation time must increase without skipping a pending
evaluation-time timer. An idle engine must wait for work or cancellation.

## Decision

Select each realtime cycle as the earlier of the next evaluation-time timer and
a cycle for ready external work. Ready external work uses current wall-clock
time, advanced when necessary to preserve increasing evaluation time. This is
required to establish a separate cycle at every pending exact timer time.
See [realtime evaluation](../design/evaluation.md#realtime-evaluation).

Use the existing engine wakeup for queue publication and a `std::stop_token`
callback. The callback only notifies the wakeup. The engine thread applies
cancellation between complete outer cycles and closes queue admission before
destroying the wakeup. Signal handling belongs to the application. These
operations require no additional runtime-graph state.
