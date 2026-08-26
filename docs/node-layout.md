# Node object layout

Status: working design. Open decisions are listed at the end.

## Goal

Production use can create many runtime graphs with the same nodes and
connections. Each runtime graph processes different data.

Store immutable descriptions, implementation code, scalar values, and
connections once. Store only mutable evaluation state in each runtime graph.

## Data placement

| Data | Location | Copies |
| --- | --- | --- |
| Parameter and output descriptions | node signature | One per distinct signature |
| Evaluation, start, and stop code | node template | One per distinct implementation |
| Resolved parameter types | wired node | One per distinct wiring |
| Scalar values supplied by graph code | wired node | One per distinct wiring |
| Input source records | wired node | One per distinct wiring |
| Runtime storage offsets and scheduling data | compiled node | One per distinct wiring |
| Output time series | runtime node | One per runtime node |
| Optional error time series | runtime node | One per runtime node with exception catching |
| Mutable implementation state | runtime node | One per runtime node that requires it |

Keep names and type descriptions in shared node data.

## Node signature

A node signature contains:

- the name and type of each node parameter
- whether wiring infers or constrains each parameter category
- the activity and validity constraint used when a parameter is an input
- the output time series type
- the information needed to resolve generic types

The data is immutable.

## Node template

A node template contains:

- a reference to its node signature
- the evaluation implementation
- the required-input invalidation behavior
- the start implementation
- the stop implementation

The data is immutable.

## Wired node

A wired node contains:

- a reference to its node template
- its resolved node signature
- scalar values supplied by graph code
- one source node index for each input
- whether wiring enables exception handling

The data is immutable after graph construction. It describes logical wiring.

## Compiled node

A compiled node is produced after the complete wired graph has been processed.
It contains:

- a reference to its wired node
- the evaluation entry point selected by wiring and the resolved start and stop
  entry points
- output and private-state offsets
- parameter packing modes and scalar or direct-input offsets
- reference-bound input slot offsets and binding modes
- active-input scheduling and input-validity data
- ranges into shared scheduling data and records for reference-bound inputs

The data is immutable and shared by runtime graphs with the same wiring. Keep
variable-length records in graph-level arrays. Store only their ranges in each
compiled node.

## Runtime node

A runtime node contains only data that can differ between runtime graphs:

- its output time series
- private mutable state required by its implementation
- evaluation state that cannot be held once by the runtime graph or derived
- an error time series when wiring enables exception handling

## Runtime graph

A runtime graph contains:

- one reference to the shared compiled-node data
- one packed state allocation

Compiled nodes and their runtime state use the same order as wired nodes. Their
positions map each runtime state to its compiled node.

## Packed runtime storage

Each runtime graph packs fixed output, private implementation, and
reference-input state into one aligned allocation:

- calculate output, private-state, and reference-input slot offsets after the
  complete graph has been wired
- store those offsets with the compiled-node data
- allocate one state block for each runtime graph
- construct each output and private state at its calculated offset

A runtime node can be a temporary view of a compiled node and its state in the
runtime graph.

The packed block contains fixed output state. Dynamic collection items and inner
runtime graphs use separate storage owned by their parent objects.

## Invocation

Invocation passes the runtime storage base, compiled-node data, and
`EvaluationContext&` to the raw function pointer selected during graph
compilation.

The function uses compiled offsets to locate inputs, output, and private state. It
then calls the typed implementation.

For a plain C++ function, compiled parameter records retain callable order and
identify shared scalar data, a direct runtime input, or a reference-bound
runtime input. The evaluation entry point uses these records to pack the C++
arguments. Runtime graphs share the records and scalar values.

## Concurrency

Different runtime graphs can run concurrently while sharing immutable node
signatures, node templates, wired nodes, and compiled nodes. Each runtime graph
has its own output time series and private implementation state.

One thread evaluates a runtime graph at a time.

## Open decisions

1. The interface for specifying private-state size, alignment, construction,
   and destruction.
2. Ownership of node signatures, node templates, wired nodes, and compiled
   nodes.
3. Whether runtime graphs cache each node's number of invalid required inputs
   and where they check that state.
