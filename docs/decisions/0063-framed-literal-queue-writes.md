# Queue messages use bounded tick payloads

Status: accepted

## Decision

The writer projects a literal before reservation, then traverses it again to
emit the existing tick grammar. Projection validates selectors and computes
the bounded frame size without constructing a temporary selector structure.
See the [queue message format](../time-series-types.md#queue-messages).

## Reason

Projection computes the complete reservation size before writing payloads.
Provider scalar codecs project the same values and field alignment as their
write handlers. A scalar conversion can still fail during emission. A failed emission
publishes a sentinel frame with the projected size, preserving the position of
later reservations. Cleanup destroys each completed typed payload before
publishing that frame. The writer moves each owning `ScalarValue<>` into the
reservation so the queue owns exactly one instance of each scalar payload.

Typed frame headers use the buffer's maximum supported alignment. This is
required to project provider typed fields from a known aligned position while
preserving the existing tick payload grammar. Readers use typed header reads
to skip alignment at a block boundary.
