# Input change queries apply to the current evaluation

Status: accepted

## Context

Invalidation is a time series change. Input queries must distinguish a change
in the current evaluation from the latest stored change, including when another
input schedules the node.

## Decision

Input change queries follow the [node input API](../node-api.md#complete-time-series-access).
The existing input modification flag includes invalidation and filters stored
validity transitions. `TimeSeriesState` continues to describe the latest stored
change.

## Consequences

Optional inputs require a validity check before value access, even when modified.
The implementation adds no runtime storage or per-cycle reset work.
