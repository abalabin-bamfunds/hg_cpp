# Shared reference transition samples

Status: superseded

## Context

TSS and TSD reference transitions expose changes by comparing their previous
and current targets. Subscribers of the same transition and requested
representation need the same change view.

## Decision

Cache one lazily populated sample per publisher output, previous and current
references, and requested runtime type. Keep it for the outer cycle, then clear
it after cycle callbacks release receiving input pointers.

## Consequences

Subscribers reuse one membership or key comparison, and change data is
materialized only when queried. Each receiving input stores one sample pointer.

See [reference sampling](../design/time-series-references.md#input-sampling).

Replaced by [receiving-input reference sample ownership](0039-receiving-input-reference-sample-ownership.md).
