# Periodic callback clock polling

Status: accepted

## Context

Reading the steady clock after every outer cycle adds substantial overhead to
small graphs. Python runners always install a periodic callback to process
signals, so this cost applies to ordinary Python simulation runs.

## Decision

Amortize clock reads across eight complete outer cycles during continuous
evaluation. Keep the countdown in the engine run's callback runner. Check the
deadline without that countdown before and after every realtime wait. This is
required to preserve callback wakeups when no graph cycle is ready.

The cycle limit bounds additional work before a deadline check; it does not
bound callback lateness in wall time. See
[periodic callbacks](../evaluation.md#periodic-callbacks) for the timing contract.
