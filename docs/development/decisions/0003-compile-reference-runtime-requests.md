# Compile reference runtime requests

Status: accepted

## Context

A reference-making node can advertise a requested target type while its root
still points to a different compatible storage shape. Reference notification
must select any required representation without running wiring type resolution.

## Decision

Graph compilation stores the requested target runtime type in each dereferencing
input notification. The reference subsystem compares runtime types and delegates
adaptation to the requested runtime type's reference representation factory.

Runtime types expose whether they store a reference through a packed property.
Reference processing uses this property and established typed boundaries instead
of runtime downcasts.

## Consequences

Reference notification performs no semantic type matching, type-map
construction, or runtime downcasts. Reference subscriber and runtime-type
object sizes do not increase.
