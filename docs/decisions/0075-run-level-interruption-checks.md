# Run-level interruption checks

Status: accepted

## Context

Python processes signal handlers on its main thread. Native graph execution
releases the GIL, and an idle realtime engine can wait indefinitely. Python
therefore needs execution opportunities while the engine owns that thread.

## Decision

Pass an optional interruption callback and interval through the run options to
both evaluation engines. Check between complete outer cycles and bound realtime
waits by the next check deadline. Use the steady clock so simulation time does
not affect check frequency. Check deadlines do not create graph cycles.

The Python runners supply a callback that acquires the GIL and calls
`PyErr_CheckSignals()`. Callback exceptions use ordinary graph cleanup before
propagating. This preserves Python signal-handler policy and keeps Python
dependencies in the bindings. See [interruption checks](../evaluation.md#interruption-checks).

Keep check state in the engine run. Runs without a callback retain their existing
wait behavior and require no clock reads for interruption checks.
