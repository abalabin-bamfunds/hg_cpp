# Reference sample runtime hooks

Status: accepted

## Context

Dereferenced inputs need sampled views for collection changes and nested item
changes. Providers must be able to define those views for their own time series
families.

## Decision

The requested `TimeSeriesRuntimeType` supplies an optional reference sample
factory. The factory receives the requested semantic type and the resolved
previous and current sources. A returned sample exposes the requested semantic
type and uses each source's actual runtime type to interpret its storage.

## Consequences

Provider-defined time series can opt into reference sampling without adding
their semantic type to BG's sampling dispatch. Samples retain the existing
receiving-input lifetime and may borrow source storage through that lifetime.

See [reference sampling](../design/time-series-references.md#input-sampling) and
[time series providers](../design/time-series-providers.md#runtime-types).
