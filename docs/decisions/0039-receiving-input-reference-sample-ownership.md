# Receiving-input reference sample ownership

Status: accepted

## Context

TSS and TSD reference notifications share sampled change data among inputs.
The sample must remain available for the receiving inputs' cycle, but a
reference provider or outer runtime graph should not own samples for inputs in
nested graphs.

## Decision

Each reference notification creates a temporary lookup for sharing samples
among its subscribers. The lookup key includes both references, the requested
runtime type, and the resolved previous target state. Each receiving input
retains its sample until the next outer-cycle callback. The same callback
releases it if that input's graph stops or is finalized first.

## Consequences

Reference sample lifetime follows the graph that owns each receiving input.
`RuntimeGraph` handles only generic cycle callbacks and has no reference-sample
cache or reference-input sweep.

See [reference sampling](../time-series-references.md#input-sampling) and
[deferred cleanup](../evaluation.md#deferred-cleanup).
