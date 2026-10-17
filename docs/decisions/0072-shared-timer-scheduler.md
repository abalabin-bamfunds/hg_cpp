# Shared timer scheduler

Status: accepted

## Context

Timer cancellation needs direct record lookup. Evaluation-time and wall-clock
timers have different readiness criteria. Inner graph timers must reach the
outer evaluator without copying deadline changes through every owner.

## Decision

The outer `RuntimeGraphEvaluator` owns one lazy timer scheduler. It stores
records in `sparse_bunch`, tracks occupied and fired positions, and keeps
separate indexed heaps for the two timer kinds.

Reuse the root evaluator's owner union for the scheduler pointer and each
timer-enabled graph's existing timer word for its owner-list head. Graph stop
removes its records through the existing lifecycle path. This preserves runtime
graph and evaluator sizes and requires no timer cleanup callback.

Handles contain a pool position and a scheduler-wide sequential 32-bit ID. Zero
is invalid; counter wrap is accepted. Reused positions therefore require no
persistent generation array. Fired records remain live through the outer cycle
so nodes can query activation without additional per-node state.

Context timer operations use the root references in
[`GlobalEvaluationContext`](0071-global-evaluation-context.md). See
[timer behavior](../evaluation.md#timed-scheduling) and
[inner graph scheduling](../nested-graphs.md#evaluation).
