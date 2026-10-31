# Valid-items TSD input view

Status: accepted

## Context

A TSD key can remain present while its item time series is invalid. A consumer
may need to enumerate only valid items and receive their validity transitions
as key changes.

## Decision

`valid_items` is an `In<TSD<...>>` guarantee. It filters the receiving input's
queries using the TSD's published changes. A reference-bound input uses its
existing shared TSD sample to determine prior item validity. The guarantee
adds no output representation or per-input item history.

## Consequences

Ordinary TSD inputs keep their existing access path and storage. Filtered
queries inspect item validity when called. `all_valid` on the filtered input
requires only a valid TSD root.

See [time series validity](../design/time-series-validity.md#collections).
