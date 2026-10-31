# Copying frame and column row lifetimes

## Decision

Full `copy()` removes the destination's current rows and creates new row
lifetimes from the source's complete current value.

Consecutive `copy_tick()` calls preserve destination row lifetimes through a
source-to-destination row mapping. A current input event initializes an invalid
destination or replaces a different source row domain. Invalidation clears the
mapping. Callers use `copy()` after skipped updates or an explicit input change.

Each destination owns its cells and row mapping. Runtime descriptors contain
no per-output copy state. Copy operations read native, borrowed, and computed
sources through the same provider input interfaces.

## Reason

Numeric row IDs can be reused after removal. A source can remove a row and
reuse its ID while a consumer suspends forwarding. Full copying must therefore
create a new destination lifetime, even when the numeric source ID matches a
previously copied row.

Independent cell storage lets downstream nodes retain the last published value
while the source continues changing. This is required for BG's sampling and
tick-selection operators.

The [integration proposal](../../projection_library_proposal.md#copying-and-retained-ticks)
defines the provider operations and verification requirements.
