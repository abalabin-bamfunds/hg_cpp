# Inline root subscriber

Status: accepted

## Context

A root reference commonly has one node subscriber. The allocated subscriber
collection and its first record use 104 bytes in addition to the 32-byte root
reference. Reference evaluation also uses the runtime type cached by the root.

## Decision

Grow the root reference to 40 bytes and store its first node subscriber
directly while retaining the cached runtime type. Promote the subscriber to the
existing allocated collection when another subscriber or an input callback is
added. Provider creation retains any subscriber stored by the root.

## Consequences

A root with one node subscriber saves 96 bytes and two allocations. Roots with
no subscriber or with promoted notification state use eight additional bytes.
Reference resolution keeps its existing runtime-type access cost.

See [time series references](../design/time-series-references.md#subscriptions).
