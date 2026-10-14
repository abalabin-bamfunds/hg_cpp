# Stable TSD representation runtime types

Status: accepted

## Context

A TSD reference representation previously owned its runtime type. Reference
samples and key selections can retain that runtime-type pointer after the
representation is destroyed.

## Decision

Share immutable TSD representation runtime types by resolved TSD type, key
runtime type, and item runtime type. A resolved representation points its value
at the representation object. The runtime type reads the current base through
that object. Key retention returns the underlying TSD runtime type and storage
that own the retained key.

## Consequences

Runtime-type pointers remain valid independently of representation lifetime.
Sampled changes and key selections use the selected representation without
requiring its runtime type to own per-representation state.

See [dynamic representations](../time-series-references.md#dynamic-representations).
