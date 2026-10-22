# Explicit reference wiring-point conversions

Status: accepted

## Context

Reference-typed context providers and type inspection need a point whose
semantic type is a reference before materialisation. Type compatibility permits
a value to bind to a reference parameter without changing the source point's
semantic type.

## Decision

Converting a value wiring point to `REF<TS_T>` returns a deferred wiring point
with a resolved reference type. Conversion validates that the source provides
a compatible direct or advertised form. Materialisation uses that source form
and inserts or reuses the ordinary reference-making node when a value form
requires it. Compatible reference wiring points retain their identity.

See [typed wiring-point conversions](../graph-wiring.md#wiring-points).

## Consequences

Explicit reference conversion gives graph code a reference-typed point without
running materialisation callbacks or adding reference-making nodes before demand.
Value consumers use ordinary reference-to-value binding. Optional and
`valid_items` annotations survive the conversion.
