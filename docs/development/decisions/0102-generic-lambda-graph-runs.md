# Generic lambda graph runs

Status: accepted

C++ `run_graph` accepts an inline generic lambda whose parameters receive graph
connections. The runner infers scalar stream types from the supplied values so
simple graph code can use `auto` before it needs an explicit time series contract.
Fixed configuration belongs in captures or helper arguments.

This extends the [unified graph runner](0082-unified-graph-runner.md) with input
inference for generic C++ callables. Concrete declarations retain their existing
argument interpretation.

Inference uses a typed C++ range's element type or consistent scalar literal values.
Empty literal streams, sentinel-only literals, and collection literals use explicit
input descriptors. This is required to select a type and collection shape without
an arbitrary default.

The [direct execution contract](../design/graph-wiring.md#direct-execution)
defines argument interpretation and its relationship to concrete declarations.
