# Dereferenced target identity

Status: accepted

## Context

Different reference handles can resolve to the same time series. A handle
replacement must retain the new subscription path without sampling that time
series again. Explicit reference inputs still observe ordinary reference ticks.

## Decision

Each dereferencing node input compares handles, then resolved storage and
runtime type, and schedules according to changes in the selected time series.
Handle replacement retains the new handle and updates subscriptions even when
the selected target is unchanged.

The input slot remains one word. When suppressing a same-target reference tick
requires preserving the input's last-updated time, tag `11` selects an allocated
two-word block containing the actual reference and retained update state. The
next input resolution restores the direct reference and frees the block when
the target's last-updated time is later than the reference's current publication
time. Target replacement and binding destruction also release it.

Free TSL and TSB item access exposes the referenced target's state. This is
required to preserve source update times when a nested graph rebuilds a free
collection from existing time series. Active collection samples retain their
rebinding changes until cycle cleanup.

## Consequences

The input's visible last-updated time is independent of same-target reference
ticks. Inputs that do not need retained update state require no additional
allocation. Graph wiring and explicit reference inputs keep their existing
behavior.

See [reference subscriptions](../design/time-series-references.md#subscriptions).
