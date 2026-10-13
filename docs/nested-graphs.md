# Nested graphs

## Purpose

A nested graph lets a node own and evaluate another runtime graph. It preserves
the evaluation time, ordering, and lifetime rules of the outer graph while the
inner graph has an independent lifecycle.

## Graph boundary

An inner graph has an explicit signature and a separate `GraphWiring`. Its graph
code uses boundary wiring points created from that signature.

Graph compilation recursively compiles the inner wiring. The outer compiled
graph owns the resulting inner compiled graph. Each runtime instance of the
owning node creates its own inner runtime graph from that compiled graph.

The [evaluation model](evaluation.md#evaluation-model) includes dependencies
that cross the boundary. Inputs enter an inner graph through references from
upstream time series. When an inner graph has an output, it crosses the boundary
as a reference and can identify a time series owned by the inner graph or an
outer graph. Reference routing follows the [time series reference
design](time-series-references.md#nested-graph-boundaries).

## Wiring

`nested(graph, run, arguments...)` creates a node whose parameters follow the
graph signature. `run` is a required `TS<bool>`. The node starts an inner graph
while `run` is true and stops it when `run` becomes false.

Time series arguments cross the boundary as references. Scalar arguments become
immutable data in the inner compiled graph. The node output is a reference to
the graph output.

For a non-reference outer graph output `T`, the inner graph input has type
`REF<T, const>`. It identifies that output for the inner graph's lifetime. An
outer graph reference output keeps its complete type, including its reference
guarantee and target type.

Mapped TSD items use `REF<V, ordinary>`, where `V` is the item's target type.
With several mapped inputs, removing and reintroducing an item in one input can
replace its time series while another input keeps the inner graph alive.
Dereferencing follows the [reference guarantee rules](type-system.md#reference-guarantees).

An inner graph records each [context time series](graph-wiring.md#context-time-series)
that its graph code requests from an enclosing lexical binding. Wiring resolves
the binding at once and imports its provider through nested boundaries as a
passive input. Compilation orders each provider before the nodes that consume
it.

Before an inner graph starts, its owner places the provider references in a
child evaluation-context section. `RuntimeGraph::start()` initializes each
compiled context slot with its reference. Wiring records the input binding
action for each use of a slot. A direct value or const reference needs no
forwarding node in the inner graph.

A context supplied by a reference whose target can change crosses each
boundary through an engine-only `REF<REF<T>>`. The reference-making node in the
outer graph observes changes to the supplied reference. An inner forwarding
node binds through the context slot, subscribes to those changes, and publishes
the current `REF<T>`. Consumers bind to that output through ordinary reference
inputs. This keeps target changes observable in the current outer cycle, even
when the consumer is several graph levels below the provider.

The owning `nested`, `map`, `mesh`, or `switch_` node does not observe or
forward context changes. Graph structure identity includes each context name
and type but excludes the provider node identity. Equal inner graphs can
therefore share compiled structure across providers.

Context lookup and boundary discovery happen during wiring. A compiled graph
stores one runtime slot for each context requirement and initializes it once at
startup. A graph without context requirements allocates no context slots and
performs no context initialization. Later changes use ordinary reference
notifications.

## Evaluation

The owning node invokes the inner runtime graph as part of the
[outer cycle](evaluation.md#outer-cycle). The inner graph therefore uses that
cycle's evaluation time and global state. The once-per-cycle evaluation limit
applies across all graph levels.

The inner runtime graph receives a child of the owning node's evaluation
context. The child changes the current runtime graph and node while preserving
the outer-cycle services and inherited context sections. An owner adds a
section by deriving another child context before it evaluates the inner graph.

Scheduling produced while an inner graph is evaluating remains within that
evaluation and is processed before the inner graph returns.

Scheduling an inner node from outside its current evaluation schedules the
owning node. This propagation repeats through each containing graph. The owning
node then evaluates the inner graph at the current outer-cycle time.

An inner graph mirrors its earliest scheduled time to its owning node.
Adding or cancelling inner schedules updates that time. This allows the outer
evaluation engine to select the next outer cycle without inspecting inner
scheduling storage.

When one node owns several inner graphs, each inner graph reports an
owner-supplied instance token with immediate scheduling and changes to its
scheduled time. The owner uses these notifications to track ready instances and
its earliest scheduled time. It does not inspect every inner graph.

The [`map` operator](map.md) uses this interface to own one inner runtime graph
per current key. The [`switch_` operator](switch.md) uses it to own the selected
inner runtime graph. The [`mesh` operator](mesh.md) can pause one inner graph
while it evaluates another inner graph selected by key, then resume the paused
graph in the same outer cycle.

Each inner runtime graph keeps its own storage, allocator, scheduling, and
lifecycle state. A `map` or `mesh` owner shares one `RuntimeGraphEvaluator` across
its inner graphs for the parent connection.

When an inner graph reports [pending evaluation](evaluation.md#pending-evaluation),
an owner that cannot satisfy it returns pending unchanged. This applies to
ordinary nested graphs, `map`, `switch_`, and nested meshes.

## Lifecycle

An inner runtime graph can start after its outer graph and stop before it. Its
start time is the current outer-cycle time. One runtime graph instance starts
once; a later start creates a new instance from the same compiled graph.

Stopping an inner graph runs its stop functions and discards its scheduled work,
timed scheduling, and cycle callbacks. Its boundary references retain their
targets while inactive, and their changes are ignored. A new instance samples
the boundary inputs when it starts.

The stopped graph remains allocated through the current outer cycle. It is
destroyed before work begins in the next outer cycle. This is required to keep
references to its time series valid through the cycle in which it stops. Stopping
the outer graph stops its remaining inner runtime graphs. Their storage is
destroyed with the outer runtime graph.

An exception from an inner graph propagates through its owning node and stops
the outer graph, as defined by [Exceptions](evaluation.md#exceptions).
