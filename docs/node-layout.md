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
| Parameter and output descriptions | signature | One per distinct signature |
| Evaluation, start, and stop code | node template | One per distinct implementation |
| Resolved parameter types | wired node | One per distinct wiring |
| Scalar values supplied by graph code | wired node | One per distinct wiring |
| Input source records | wired node | One per distinct wiring |
| Runtime storage offsets and scheduling data | compiled node | One per distinct wiring |
| Output time series | runtime node | One per runtime node with an output |
| Optional error time series | runtime node | One per runtime node with exception catching |
| Mutable implementation state | runtime node | One per runtime node that requires it |

Keep names and type descriptions in shared node data.

## Signature

A signature contains:

- the name and type of each parameter
- whether wiring infers or constrains each parameter category
- the activity and validity constraint used when a parameter is an input
- whether a scalar parameter accepts absence
- zero or one output time series type
- the information needed to resolve generic types

The data is immutable.

## Node template

A node template contains:

- a reference to its signature
- the evaluation implementation
- whether its invocation requires the output runtime type
- private-state size, alignment, and initial-value construction when required
- the required-input invalidation behavior
- the start implementation
- the stop implementation

The data is immutable.

`NodeTemplate` is a polymorphic interface used during wiring and graph
compilation. Aggregate classes, C++ callables, and Python definitions use
separate implementations. Graph compilation selects the raw entry points used
for evaluation.

## Wired node

A wired node contains:

- a reference to its node template
- its resolved signature
- scalar values supplied by graph code
- one source node index for each input
- whether wiring enables exception handling

The data is immutable after graph construction. It describes logical wiring.

## Compiled node

A compiled node is produced after the complete wired graph has been processed.
It contains:

- the evaluation entry point selected by wiring
- output offsets when present, and private-state offsets
- parameter packing modes and scalar or direct-input offsets
- reference-bound input slot offsets and binding modes
- active-input scheduling and input-validity data
- required-input invalidation mode and an optional function entry point
- subscribers and optional output-notification metadata
- a runtime type table indexed by parameter position when any input requires runtime metadata
- declared and runtime type pointers for `all_valid` input checks, independent of input-view construction metadata
- the selected runtime type pointer for an output whose layout is selected during graph compilation
- optional data required by a runtime-signature node definition
- immutable callable captures and their destruction function when present

The data is immutable and shared by runtime graphs with the same wiring. Each
compiled node occupies one aligned section of the compiled graph's fixed
allocation. Its parameter bindings, subscribers, and fixed scalar objects
follow it in the same section. Sections use topological order.

Nodes requiring runtime or `all_valid` metadata store an eight-byte runtime
header immediately before `CompiledNode`. Runtime pointers precede that header
in reverse logical order. Logical slots contain parameters, an optional output,
then dynamic references. A parameter slot is therefore at a fixed indexed
offset before the node. The header stores the complete pointer count and the
first dynamic reference slot. `all_valid` descriptors and their count precede
the runtime pointers.

The table reserves one slot per parameter only when input runtime metadata is
required. Unused parameter slots contain null pointers. Nodes without runtime
or `all_valid` requirements store no runtime header or table.

Graph-level arrays contain node lookup, start, stop, destruction, and diagnostic
data. Evaluation does not load these arrays unless it needs the corresponding
function.

Compilation copies scalar values and leaves graph wiring reusable. Variable
scalar data uses a separate allocator owned by the compiled graph.

## Storage cleanup

A runtime type reports variable-memory use and
[finalization](terminology.md#finalization) separately. A collection runtime
type requires finalization when a contained runtime type requires it.

Destroying one value performs its finalization and releases its variable
memory. A graph can discard its complete allocation without destroying
values that do not require finalization.

## Runtime node

A runtime node contains only data that can differ between runtime graphs:

- its output time series when declared
- private mutable state required by its implementation
- evaluation state that cannot be held once by the runtime graph or derived
- an error time series when wiring enables exception handling

## Runtime graph

A runtime graph contains:

- one reference to the shared compiled-node data
- the first scheduling block, covering up to 64 nodes
- one packed state allocation

The first scheduling block is stored in the runtime graph object. Any additional
blocks follow the packed node state in its allocation. Compilation stores the
initial scheduling blocks and node state in one fixed image. Runtime graph
creation copies each to its runtime location.

Compiled nodes and their runtime state use the same order as wired nodes. Their
positions map each runtime state to its compiled node.

## Packed runtime storage

Each runtime graph packs fixed output, private implementation, and
reference-input state into one aligned allocation:

- calculate output, private-state, and reference-input slot offsets after the
  complete graph has been wired
- store those offsets with the compiled-node data
- initialize one runtime storage block during graph compilation
- allocate one state block for each runtime graph and copy the initial block

The initial runtime storage contains position-independent data. Relationships
use offsets or indices. Every runtime type selected for this storage supports
byte-copy initialization. After the copy, node start functions perform
graph-specific startup work.

One node's private-state fields share an aligned range and one compiled base
offset. Field offsets within the range are compile-time constants.

`Node` is a two-pointer view of a compiled node and its state in one runtime
graph. It resolves compiled offsets to runtime addresses.

The packed block contains fixed output state. Dynamic collection items and inner
runtime graphs use separate storage owned by their parent objects.

## Invocation

Invocation passes the runtime storage base, compiled-node data, and
`EvaluationContext*` to the raw function pointer selected during graph
compilation.

The function uses compiled offsets to locate inputs, output, and private state. It calls the typed implementation when
the node defines one.

For a plain C++ function, compiled parameter records retain callable order and
identify shared scalar data, a direct runtime input, or a reference-bound
runtime input. The evaluation entry point uses these records to pack the C++
arguments. Runtime graphs share the records and scalar values.

C++ callables use compile-time parameter positions and activity-control
indices. Their compiled nodes omit parameter names, type descriptions, and the
parameter-to-activity-control lookup. They retain runtime type pointers only
when constructing a declared view requires them. A capturing callable stores
its immutable function object and required destruction function once in compiled
data. An empty, default-constructible callable stores no function object there.

For an aggregate class declaration, the evaluation entry point constructs a
temporary class value. It binds each `In<>` field from the corresponding
compiled parameter record, copies each scalar field from compiled scalar data,
binds `State<T>` to private runtime state, and binds the `Out<>` field to the
runtime output. It then calls the typed `evaluate()` method when present. The method can be inlined into the entry
point, and the temporary value can be replaced by its individual fields during optimisation.

For a runtime-signature declaration, the evaluation entry point creates a
`NodeInvocation`. It exposes lazy ranges over input and scalar parameter
bindings and binds views as the implementation requests them. Its compiled
node retains parameter names, resolved types, and activity-control lookup data
for runtime inspection.

## Concurrency

Different runtime graphs can run concurrently while sharing immutable node
signatures, node templates, wired nodes, and compiled nodes. Each runtime graph
has its own output time series and private implementation state.

One thread evaluates a runtime graph at a time.

## Open decisions

1. Ownership of signatures, node templates, wired nodes, and compiled
   nodes.
2. Attribution of wiring and compilation allocations to a graph and node.
