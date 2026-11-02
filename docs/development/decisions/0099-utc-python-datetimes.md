# UTC Python datetimes

Status: accepted

## Decision

Interpret timezone-naive Python `datetime` values as UTC. Scalar values,
evaluation clocks, scheduling, run limits, and captured event times use one
conversion between UTC calendar fields and signed microseconds since the Unix
epoch. Conversion preserves microseconds before and after the epoch and rejects
timezone-aware values.

## Reason

The meaning of a timestamp must be independent of the machine's timezone.
Calendar conversion also avoids the platform range limits of C local-time
functions, including Windows' rejection of negative timestamps.

See the [Python node types](../design/python.md#python-nodes) and
[scalar reference](../../user/reference/scalars.md#built-in-mappings).
