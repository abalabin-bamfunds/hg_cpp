# Test graph captures serialized ticks

Status: accepted

## Decision

`TestGraphResult` owns captured output deltas in a shared buffer chain. The
runtime sink serializes each output tick with `TimeSeriesTickWriter`. Built-in
literal comparison advances a borrowed reader without moving or destroying
encoded values. Indexed scalar access returns an owning `ScalarValue<T>` copy.

Provider-defined time series types use their registered tick write and discard
operations. They register `TickLiteralComparison` by semantic time-series type
to compare valid literal payloads. The framework handles invalid ticks before
calling the comparison handler. See the [test graph result contract](../testing.md).

Scalar expectations use the existing scalar literal construction operations,
including provider handlers. Comparison consumers interpret only the sentinel
instructions defined for their positions and pass remaining literals unchanged
to scalar construction or extension comparison.

## Reason

Serialized capture preserves the runtime tick format and its provider
extensions. Borrowed comparison avoids reconstructing a second delta tree.
Owning indexed copies can outlive the result and its buffer chain.
