# Timers take priority over queue input

Status: accepted

## Decision

The runtime graph combines committed queue readiness with timed scheduling in
`next_scheduled_time()`. At outer-cycle entry, it checks actual timer records
before activating them and admits queue input only when no timer is due. See
[queue input](../design/evaluation.md#queue-input).

## Reason

Pending messages must wait while graph work schedules minimum ticks. Checking
timer records before activation preserves that priority without recording a
separate scheduling category. Reporting queue readiness through the existing
scheduling interface lets simulation continue pending messages without another
timer or a separate engine loop.
