# Scalar layout capability and compiled runtime slots

Status: accepted

## Decision

Scalar storage traits explicitly declare whether typed reads support alternative
layouts. Graph compilation assigns runtime metadata slots by parameter position
and records the positions needed for output and dynamic reference access.
Runtime pointers precede the compiled node in reverse logical order, with their
header immediately before the node.

Alternative-layout providers with reference-valued read types supply a typed
`read_value(ScalarData)` hook that returns a source-owned reference.

See the [scalar provider contract](../design/scalar-api.md#scalar-providers) and
[compiled node layout](../proposals/node-layout.md#compiled-node).

## Reason

A scalar provider knows whether its `read()` operation can interpret every
stored representation. Read and copy hooks describe conversion and ownership;
their presence does not determine layout compatibility.

The runtime read operation constructs a C++ value in caller-owned storage.
A reference read requires the provider to identify an existing source-owned
object rather than a materialized temporary.

Compilation knows each resolved parameter position. Preparing metadata there
keeps evaluation access to one indexed runtime pointer load and preserves the
small parameter binding and compiled node headers. Fixed offsets from the node
locate runtime pointers across provider calls. Unused slots increase compiled
storage when runtime metadata requirements are sparse: a node with `P`
parameters and `K` required input runtimes stores `P - K` unused pointers.
Runtime graphs share that storage.
