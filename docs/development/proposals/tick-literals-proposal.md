# C++ tick construction and application

Status: proposal for discussion.
The [scalar API](../design/scalar-api.md#literals), [output API](../design/node-api.md#output-parameters),
and [test utilities](../testing.md#test-layout) define the implemented literal interfaces.
Collection test results use deltas. Concurrent writes measure their payloads,
write directly into reservations, and publish failed reservations with recipient
`0xFFFFFFFFu` after cleanup.

## Purpose

Use one C++ brace syntax to apply output updates, return updates from callable
nodes, compose wiring points, construct constants, write retained records, and
supply test inputs.
Support runtime-defined types and template declarations, including recursively
nested scalar and time series collections.

The required interfaces are:

- `output.apply({{"a", 2}})`
- `return Tick({{"a", 2}})`
- `wp<TSB<Schema>>{{"a", wp_x}, {"b", wp_y}}` and typed graph brace returns
- `const_[type<TSD<string, TS<i64>>>](Literal{{"a", 2}})`
- writing literal data into sequential and concurrent buffers
- one `run_graph(graph, input1, input2, ...)` call with brace sequences

`Literal`, `Tick`, `measure_write`, and the operation marker names below are
proposed API names. This proposal does not change the
[terminology dictionary](../../terminology.md).

## Common syntax

Propose a borrowed `Literal` argument that represents one of:

- a scalar source, including an existing `Scalar` or `ScalarValue`
- a recursive `std::initializer_list<Literal>`
- a wiring point used by a wiring consumer
- an operation marker

Use one recursive sequence constructor. Interpret entries through the expected
type rather than selecting separate list and dictionary constructors. This is
required to accept heterogeneous nested braces without ambiguous constructor
overloads.

For example, `{{"a", 2}}` contains one two-element sequence. The expected type
determines whether that sequence describes a scalar dictionary entry, a time
series dictionary update, or another supported structure.

`Literal{}` is an empty sequence. `Literal{1}` contains one scalar;
`Literal(1)` is a scalar leaf. `Literal{{}}` contains one empty sequence.
Consumers preserve these shapes when interpreting nested values.

Scalar sources retain their source type and representation. Interpretation
uses the existing [scalar construction rules](../design/scalar-api.md#c-source-values).
An explicit scalar view or owner retains its semantic type. Unsupported
conversions produce an error; a destination type never authorizes interpreting
a source address as a different C++ representation.

A wiring-point leaf retains the graph-owned wiring identity. Wiring consumers
connect that point; value consumers reject it. `Tick` owns value data and rejects
wiring-point leaves. Update markers are accepted by update consumers, with the
restrictions below; wiring composition and constant construction reject them.

## Template and runtime typing

Templates associate a declaration with an operation. Runtime metadata supplies
the resolved type and physical representation.

```cpp
// The output supplies its actual runtime type, including for Out<TS_T>.
output.apply({{"a", 2}});

// The existing operator type binding supplies the constant's value shape.
auto constant = const_[type<TSD<string, TS<i64>>>](Literal{{"a", 2}});

// A typed tick supplies the node output declaration.
auto tick = Tick<TSD<string, TS<i64>>>({{"a", 2}});

// Runtime-typed ownership; type is a resolved TimeSeriesType.
auto runtime_tick = Tick<>(type, {{"a", 2}});

// Ownership with interpretation deferred to the consumer.
auto unbound_tick = Tick({{"a", 2}});

// Typed and runtime-typed record writing.
writer.write<TSD<string, TS<i64>>>({{"a", 2}});
writer.write(type, {{"a", 2}});
```

`Literal` contains source syntax; its consumer supplies the expected type.
`Tick<TS_T>` exposes `time_series_type` for signature inference. An erased
`Tick<>` may retain a resolved declaration or defer interpretation. A generic
template declaration resolves through normal wiring. Application checks its
compatibility with the resolved destination.

A template declaration does not select a native storage layout. Application
uses `output.runtime_type()` and each child output's runtime type. This is
required to support alternative runtime representations and provider-defined
children. Follow the existing [runtime-selected replay
rule](../decisions/0032-runtime-selected-tick-replay.md).

An untyped brace expression cannot identify the output type of an erased node,
an untyped buffer, an erased wiring constructor, or a constant. Those boundaries
must provide a resolved type or obtain it from wiring resolution. For example,
`{{"a", 2}}` alone cannot select TSD rather than `TS<Dict<string, i64>>`.

## Proposed update semantics

Use selected changes as the default for collection time series. Scalar
collections remain complete scalar values.

| Expected type | Interpretation |
| --- | --- |
| `TS<T>` | Publish one complete scalar value |
| `TS<List<T>>` | Construct and publish the complete scalar list |
| `TS<Dict<K, V>>` | Construct and publish the complete scalar dictionary |
| `TS<Set<T>>` | Construct and publish the complete scalar set |
| Fixed TSL | Apply child updates by position |
| TSB | Apply child updates by field name |
| TSD | Apply child updates by key |
| TSS | Insert listed items and erase explicitly removed items |

The proposed markers are:

- `SKIP`: perform no operation at this position
- `INVALID`: invalidate this time series through its output API
- `REMOVE`: erase the enclosing TSD key
- `REMOVED(value)`: erase a TSS item

`REMOVE` is accepted only as a TSD entry value. `REMOVED(value)` is accepted
only as a TSS operation. Time series markers are rejected inside scalar values.
Invalidation follows the [validity rules](../design/time-series-validity.md); TSD and TSS
root invalidation is rejected.

Omitted TSD keys and TSB fields retain their current state. Omitted TSL suffix
positions retain their state; `SKIP` preserves an interior position. Reject
positions beyond the declared fixed size and unknown TSB fields.
Skipping a TSD entry preserves absence when its key is not present; application
must check the marker before accessing a child output that would create the key.

An empty TSD or TSS sequence establishes first-tick validity. Applied to an
already-valid collection, it preserves its value. An empty TSL or TSB sequence
performs no child updates. Publishing an empty scalar collection replaces its
complete scalar value.

Apply entries in their written order. Publication and change tracking follow
the normal `Out` operations, including repeated updates in one invocation and
output guarantees. The operation adds no timestamp to the supplied data;
application uses the destination's evaluation time.

Whole-collection clear and replacement remain open scope decisions. The
existing delta record grammar cannot erase unspecified current keys without a
matching baseline. Supporting these operations across consumers requires an
explicit encoding decision. Replacement also needs a rule for publishing
retained items.

## Nested application

For `TSD<string, TSB<Schema>>`, suppose `Schema` declares a `price` field of
type `TS<i64>` and a `children` field of type `TSD<string, TS<i64>>`:

```cpp
output.apply({
    {"order", {
        {"price", 100},
        {"children", {{"a", 10}, {"b", 20}}},
    }},
});

output.apply({
    {"order", {
        {"children", {{"a", 11}, {"b", REMOVE}}},
    }},
});
```

The second application updates child `a`, removes child `b`, and preserves
`price`. The interpreter selects the outer dictionary item, the bundle field,
the inner dictionary item, and its scalar output recursively. Each selected
output retains its containing output path. Leaf operations therefore update
parent change tracking through the existing output machinery.

Errors identify the nested key, field, or index and the expected type.
Application follows [node failure behavior](../design/evaluation.md); it does not promise
rollback of mutations already performed before a later operation fails.

## Ownership

`Literal` borrows initializer-list storage and borrowed scalar sources. A
consumer must finish reading it before the full expression ends. Scalar source
adapters must also handle arithmetic and other temporary values without
retaining addresses of constructor-local conversions.
Saving a `Literal` constructed from braces does not extend the backing
initializer lists' lifetimes. Use `Tick` when value data must survive the call.

Accept lvalues and rvalues of supported scalar sources, including temporary
strings and scalar owners:

```cpp
output.apply({{"a", std::string{"value"}}});
auto tick = Tick({{"a", make_value()}});
```

The original temporary remains alive until the full expression ends. Direct
application finishes within that lifetime. `Tick`, constant construction, and
retained records acquire ownership before it ends. Rvalue acceptance does not
promise ownership transfer; retention follows the scalar copy and shared-owner
contracts. Adapters must not borrow a temporary created inside the adapter.

`Tick` owns the recursive contents that must survive that expression. It is
move-only, and moving transfers ownership. Its owned scalar payloads use
`ScalarValue` and its structure uses the same allocation resource. Retaining a
borrowed source copies it through the scalar ownership APIs; shared scalar
storage follows its existing retain policy. An initializer list exposes const
elements, so construction must not promise to move owners out of its elements.

`Tick` keeps source data directly. Serialization is performed by a writer when
the consumer requests retained records. This permits an unbound or generic tick
to defer interpretation until its resolved output type is available.

`Tick::apply(output)` preserves the owned contents. Reapplying a collection
delta requires the intended destination baseline; it does not reconstruct a
complete value from an arbitrary state.

Immediate `Out::apply` traverses `Literal` directly. Concurrent record writing
measures and emits the data within the call. Required output storage, retained
scalar payloads, and scalar conversions can allocate; these operations do not
first construct an owning `Tick`.

Allocation follows the [scalar ownership contract](../design/scalar-api.md#ownership).
A returned tick may use invocation storage when application and destruction
complete in the invocation. Queued ticks use an explicit allocator whose memory
resource remains valid until their payloads are released. The queue owns thread
synchronization and evaluation scheduling.

## Consumers

### Output application

Add `apply` consistently to concrete and generic `Out` views. Accept borrowed
syntax and an owning tick. Use the actual output runtime type and the existing
scalar publication, child selection, insertion, erasure, clearing, and
invalidation operations.

Use the scalar runtime collection APIs to construct complete scalar lists,
dictionaries, and sets. Their nested elements are scalar values. At a time
series collection boundary, recurse into child outputs instead.

### Callable returns

```cpp
const auto update = node(
    [](i64 value) -> Tick<TSD<string, TS<i64>>> {
        return Tick<TSD<string, TS<i64>>>({{"a", value}});
    });
```

Extend callable result classification so a typed tick supplies its output
declaration. The framework applies the result and destroys it before the
invocation ends. Scalar and optional scalar returns retain their current
[node API rules](../design/node-api.md#plain-c-parameters).

An erased tick return requires an output declaration supplied by the node
definition or its wiring resolvers. The declaration syntax is open. Apply tick
returns under their declared guarantees; extending scalar output guarantee
inference to ticks is a separate decision.

Graph functions continue to construct graph code. They can invoke tick-returning
nodes and return their wiring points. A declared collection return type also
supports brace composition of upstream wiring points.

### Wiring-point composition

```cpp
auto fields = wp<TSB<Schema>>{{"a", wp_x}, {"b", wp_y}};
auto items = wp<TSL<TS<i64>, 2>>{wp_x, wp_y};

auto graph = [](wp<TS<i64>> wp_x, wp<TS<i64>> wp_y) -> wp<TSB<Schema>> {
    return {{"a", wp_x}, {"b", wp_y}};
};

auto list_graph = [](wp<TS<i64>> wp_x, wp<TS<i64>> wp_y) -> wp<TSL<TS<i64>, 2>> {
    return {wp_x, wp_y};
};

// bundle_type is a resolved TSB type; the same form accepts a fixed TSL type.
auto runtime = wp<>(bundle_type, {{"a", wp_x}, {"b", wp_y}});
```

Add non-explicit initializer-list constructors so a declared graph return type
accepts copy-list initialization. Use the existing [free collection
construction](../design/graph-wiring.md#wiring-points) used by `bundle()` and
`tsl()`. The active `GraphWiring` owns the resulting wiring records; `wp` remains
pointer-sized. Item values and updates come from the connected upstream outputs.

Preserve existing `wp<TimeSeries>{point}` and cross-declaration re-annotation
calls: they retain the supplied wiring identity. Collection brace construction
must distinguish these calls from composition despite C++ initializer-list
constructor precedence.

Resolve TSB entries by field name. Require each schema field exactly once;
reject unknown fields and incompatible wiring points. Require the declared
number of fixed TSL items and check each item type. Resolve generic declarations
through normal wiring. Preserve the existing reference materialization and
guarantee rules. Scalar fields follow the existing explicit TSB construction
policy and are lifted through `const_`.

Nested braces compose nested TSB and fixed TSL fields recursively. A TSD field
can receive an existing TSD wiring point or a constant constructed below.
Wiring composition describes complete connections and accepts no update markers.

### Constant construction

```cpp
auto typed = const_[type<TSD<string, TS<i64>>>](Literal{{"a", 2}});

// bindings is a TypeMap containing the resolved output type.
auto runtime = const_(bindings, Literal{{"a", 2}});
```

Add literal overloads to the existing `const_` operator. `const_` consumes the
borrowed syntax during wiring and retains the interpreted data in graph-owned
storage. The source applies the data at its start time; each runtime graph has
independent output storage. Constant construction requires no owning `Tick`
intermediate. Existing scalar overloads retain their behavior.

Use the existing [operator type binding](../design/operators.md#declaration-and-registration)
and `TypeMap` call paths. `Literal` needs no type parameter. The binding supplies
the value shape; the selected constant overload establishes recursive const
guarantees. An unbound literal call requires its output type to be resolved by
the operator. Do not infer a collection time series type from braces alone.
Extend the current scalar `const_` declaration and call preparation to accept
literal arguments and expose the time series type parameter for positional
binding. Retain normal overload resolution after preparing the literal.

The result uses recursive `const` guarantees. Require complete initial values:
all fixed TSL positions and TSB fields, and complete valid values for every
supplied TSD item. Unlisted TSD keys and TSS items are absent. Empty TSD and TSS
values are valid constants. Reject update markers and duplicate dictionary keys
or bundle fields. This is required to establish every contained const time
series at its [start time](../../terminology.md#const-time-series).

### Buffer writing and queues

Extend `TimeSeriesTickWriter` with literal and owning-tick overloads. Provide
payload measurement and direct encoding into a reservation for concurrent
queues:

```cpp
writer.write(type, {{"a", 2}});

// tick owns stable data until measurement and emission finish.
auto extent = measure_write(type, tick);
// The queue adds its routing header and padding and reserves the complete frame.
// It encodes the payload into that reservation and owns cleanup and publication.
```

Measurement computes the exact payload byte count and alignment using the same
ordered encoding operations as emission. Check structure, markers, size
overflow, and unsupported alignment before reserving. Include typed-payload
padding and establish a known payload-start alignment. Keep input and runtime
metadata valid and unchanged between measurement and emission. A root `SKIP`
produces no record and makes no reservation; skipped TSD entries produce no key
operation.

Measurement does not construct every retained payload or serialize a temporary
record. Emission writes trivial payloads directly and constructs required scalar
owners once in retained storage. Successful writes require no per-entry undo
list or owning preparation tree. Provider encodings may use preparation when
their representation requires it; this is an operation-specific contract.

Extend the scalar buffer write context to support sequential and reserved
storage. Extensible scalar and time series encoding operations need a sizing
operation consistent with writing. Existing write-only registrations continue
to support sequential writes; reject concurrent writing without the required
sizing operation before reserving. Do not invoke an arbitrary write callback
in a counting mode. The current encoding APIs have no sizing operation.

The concurrent queue protocol begins each frame with an eight-byte header,
written in this order:

- `std::uint32_t recipient`: recipient node ID, or `0xFFFFFFFFu` for a failed frame
- `std::uint32_t frame_size`: total reserved byte count, including the header,
  payload, and padding

Node-ID conversion must reject values outside the 32-bit range and the reserved
value. Compute the complete frame size with checked arithmetic and reject sizes
outside the 32-bit range before reserving. Keep recipient selection and framing
in the queue; the literal utility measures and encodes the payload.

The frame owner installs scope-bound cleanup immediately after reservation.
It initializes both header fields before writing the payload directly and
commits the complete frame on success. If emission throws, it destroys completed
owned payloads, sets recipient to `0xFFFFFFFFu`, preserves `frame_size`, commits
the reserved extent, and propagates the original failure. Cleanup and failed-frame
publication must not throw. After success, replay or discard owns payload cleanup.
The current concurrent writer cannot rewind a reservation; committing a failed
frame allows later reservations to become readable.
The sequential writer retains its completed-prefix discard and rollback behavior.

For a failed frame, the reader advances to the frame start plus `frame_size`
without interpreting its payload. The size remains valid even when payload
encoding stops before the tick's terminating markers.

Use the existing [retained record grammar](../design/time-series-types.md#retaining-ticks)
for successful payloads. The queue reader skips failed frames and framing
padding and invokes replay or discard only for successful payloads. Frame
alignment and any trailing padding must be defined consistently for measurement,
writing, and reading.

Encoded payloads continue to apply through `TimeSeriesTickReader`. Shared
interpretation resolves structure, fields, keys, scalar sources, and operation
markers for output and writer consumers. Reuse existing replay behavior rather
than constructing another encoded-record interpreter.

A queue can retain an owning tick or encoded records. It selects the retention
form and schedules graph evaluation. The construction utility owns neither
queue policy nor a graph wakeup mechanism.

### Test graphs

The [test utility API](../testing.md#test-layout) defines literal inputs.
Collection output capture described below remains proposed.

```cpp
run_graph(add, {1, 2, SKIP}, {10, 20, 30});

run_graph(dictionary_sum, {
    {{"a", 10}, {"b", 20}},
    {{"a", 11}},
    {{"b", REMOVE}},
    SKIP,
});
```

Each input argument is a sequence of cycle updates. Thin overloads with known
initializer-list parameter types accept raw braces for each supported input
count and forward to one implementation. Template argument packs cannot deduce
raw brace expressions. The supported count remains open; existing typed range
inputs continue to support arbitrary counts.

`run_graph` completes evaluation before its input initializer lists expire.
It may borrow those sequences for the call. Source compiled data borrows the
sequence, and per-runtime state stores the next cycle index. The test run keeps
all borrowed data alive until its runtime and compiled graphs are destroyed.
Each source declares the resolved graph input type. At each scheduled cycle,
it calls `output.apply` with the next literal and advances its index. It
advances past `SKIP` entries and schedules the next update at its cycle's
evaluation time. The source uses the same nested application as production
outputs.

Input types come from concrete graph parameters or resolved template
signatures. For unresolved parameters, retain an explicit annotation form:

```cpp
run_graph(generic_node,
    input<TSD<string, TS<i64>>>({
        {{"a", 10}},
        {{"a", 20}},
    }));
```

Extend capture to collection outputs. Each cycle position contains no result
when the output did not publish, or an owning `Tick` containing its published
delta. Capture actual added, removed, and updated items through the input view;
preserve invalidation and valid empty-collection events. Keep the existing
scalar result interface. Expected collection deltas use the same literal syntax
and the resolved output type for comparison.

## Implementation and validation

Keep the utility in the public time series API. Keep cycle sequencing and test
capture in `bg::testing`. One type-guided interpreter owns literal traversal;
consumers own output mutation, retained storage, and scheduling.

Use visitors for structural interpretation and extensible dispatch for
type-specific operations. A visitor selects a scalar or time series category;
collection handlers recurse through child views carrying their actual runtime
types. Template interfaces supply declarations to the same runtime
implementation. Scalar construction and output mutation use their normal
runtime APIs.

Dispatch operations independently so a provider can supply the operations its
type supports. Select exact runtime operations before semantic operations under
the provider contracts; report unsupported operations at the consumer boundary.
Reuse existing visitors and dispatch records where their contracts fit. The
design permits focused visitor types and additional dispatch operations when
required. Keep recursion independent of nesting depth and type combinations.
Evaluation uses no runtime downcasts or separate implementations for deeply
nested combinations.

First verify the proposed C++ constructors and overloads with nested braces,
empty braces, heterogeneous TSB fields, typed graph brace returns, operator
type bindings, runtime types, markers, and one and multiple inputs.
Verify template and runtime declarations select identical interpretation.

Then implement and test these boundaries:

1. Borrowed syntax and direct output application, including nested scalar and
   time series collections and alternative runtime representations.
2. Owning ticks, erased and generic declarations, move lifetime, explicit
   allocation, temporary scalar sources, and repeated application.
3. Wiring composition and constants, including nested collections, required
   fields, reference materialization, retention, and start-time guarantees.
4. Literal record writing, measured-size agreement, alignment, replay
   equivalence, provider sizing, partial-write cleanup, failed-frame skipping,
   concurrent commit ordering, discarded queued records, and successful-write
   allocation and copying costs.
5. Test source integration and callable return integration, with independent
   runtime graphs, declared output guarantees, and collection delta capture
   distinguishing no publication, invalidation, and a valid empty collection.

The initial collection scope is fixed TSL, TSB, TSD, and TSS. Sparse TSL storage
is [not implemented](../design/time-series-types.md#type-summary). Reference literals and
literal structure for extension time series types require separate contracts;
compatible provider scalar values remain valid leaf sources.

## Decisions needed before implementation

- Confirm the proposed update semantics and operation marker names.
- Decide whether whole-collection clear and replacement belong in the first
  implementation.
- Select the erased callable output declaration syntax and the brace-input
  count supported by `run_graph`.
- Confirm complete startup values and recursive const guarantees for
  `const_(Literal)`.
- Coordinate sizing, cleanup, and the eight-byte framing interface with the
  queue lane before changing shared interfaces.
