# Inherited changing-reference transport

Status: accepted

## Context

A context time series or service reply can be a reference whose target changes.
Consumers can be several graph boundaries below its provider. Each consumer
must observe target changes without scheduling unrelated inner graphs.

## Decision

Wiring transports a changing reference across an inherited graph boundary as
an engine-only `REF<REF<T>>`. The outer reference node observes the supplied
reference. A forwarding node in the consuming graph subscribes to the
transport and publishes the current `REF<T>` for ordinary input binding.

Direct values and const references use the inherited context slot without a
forwarding node. A service reply uses changing-reference transport because its
implementation is selected after client wiring.

## Consequences

Reference target changes schedule only consumers of the inherited reference.
Nested graph owners do not observe or forward those changes.

See [context boundaries](0017-context-time-series-boundaries.md),
[nested graphs](../nested-graphs.md#graph-boundary), and
[time-series references](../time-series-references.md).
