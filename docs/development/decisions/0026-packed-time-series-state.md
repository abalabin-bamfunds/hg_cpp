# Packed time-series change state

Status: accepted

## Context

An invalidation must retain its evaluation time so a delayed tick can record
and replay the transition. A timestamp alone cannot distinguish an invalidation
from an ordinary update. Adding a separate state field to every time series
would increase runtime storage, including collection items.

## Decision

Store the latest change time and validity transition in one 64-bit
`TimeSeriesState`. Reserve two high bits for an ordinary valid update, a
revalidation, an invalidation, or a never-valid state. Valid evaluation times
use positive microsecond counts that fit in the remaining 62 bits.

## Consequences

Invalidation retains its time without increasing time-series state size. A
repeated update in the same evaluation preserves a first-tick transition. The
engine rejects evaluation times outside the representable range.

See [time-series validity](../design/time-series-validity.md) and
[evaluation time](../design/evaluation.md#evaluation-time).
