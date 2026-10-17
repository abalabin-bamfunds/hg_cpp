# Queue publication wakes the engine

Status: accepted

## Context

A waiting engine needs notification when queue publication advances the
committed watermark. Failed writes can advance it by committing sentinel frames.
Retained queue handles can outlive the engine run.

## Decision

The engine owns one single-consumer wakeup containing a binary semaphore and an
atomic notification flag. The shared queue admission gate borrows its pointer.
Notifications coalesce through the flag. A successful wait acquires the semaphore
before clearing the flag; a failed timed wait leaves the flag unchanged. The
engine rechecks readiness after every wait.

Bind the wakeup before graph startup. Writers notify before leaving admission,
including during failure cleanup. Queue closure waits for admitted writers and
then detaches the pointer. This uses the existing
[queue closure protocol](0066-queue-handle-shutdown.md) to protect wakeup lifetime
without adding runtime graph fields or cleanup callbacks.

Expose committed queue readiness separately from exact timer deadlines. Keep
the combined scheduling query for simulation. See
[queue input](../evaluation.md#queue-input).
