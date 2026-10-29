# Time series types

## Purpose

The engine provides time series types for values and common collection shapes.
The [terminology dictionary](../../terminology.md) defines each type. This document
records its notation and change queries.

Collection notation accepts another collection type as `TS_T`.

Every time series type has the recursive guarantee defined by the
[type system](type-system.md#time-series-guarantees).

A guarantee is an optional template property:

```cpp
bg::TS<float, bg::TimeSeriesGuarantee::final>
```

The [validity design](time-series-validity.md) defines invalidation and
revalidation for these types.

TSS and TSD validity starts with their first tick. Invalidation removes their
current collection value and reports member removals. TSD item validity remains
independent. Sampling an invalid reference target reports the same removals.

## Type summary

| Type | Notation | Change queries |
| --- | --- | --- |
| Time series value | `TS<T>` | None |
| Time series reference | `REF<TS_T>` | None |
| Time series set | `TSS<T>` | Added and removed items |
| Time series list (fixed size) | `TSL<TS_T, Size>` | Updated items |
| Time series bundle | `TSB<Schema>` | Updated items |
| Time series dictionary | `TSD<K, TS_T>` | Added, updated, modified, and removed items |

`TSL<TS_T>` declares the semantic type for a variable-size sparse list. Its
runtime storage and node views are not implemented.

## Layout

A time series value stores its latest change state and value independently. Its
runtime type describes the value storage. A never-valid view has `MIN_DT` as
its change time. An invalidated view retains its invalidation time. The packed
state is defined by [time series validity](time-series-validity.md).

`TimeSeriesRuntimeType` is the runtime type used by `In<>` and `Out<>`. It
provides the semantic type, physical layout, contained-item access, and runtime
operations. One semantic time series type can have multiple runtime types,
including normal and free collection layouts.

`In<>` contains a runtime type pointer, a copied change state, and a data pointer.
It is valid only for the node callback; its change state describes that invocation.
`Out<>` is a one-word view of an internal mutation record. Reference resolution
supplies the current target runtime type because the target layout can change.

Owned fixed TSL and TSB storage counts valid immediate children. Item mutations
update this count, making collection validity and full-schema `all_valid()`
checks constant-time. A narrowed TSB input checks only its declared fields.

`In<>` and `Out<>` visitors select value, reference, list, bundle, set,
dictionary, or erased extension access. The selected view is valid for the
visitor callback. Fully specified views use their typed operations directly.

The [provider API](time-series-providers.md) defines how a separate library
adds declarations, runtime representations, and input/output views.

Collection storage uses the resolved runtime type of each contained time
series. Runtime-defined collection types therefore support the same contained
time series as C++-defined collection types.

Collection runtime types identify items with 32-bit item IDs. TSL and TSB use
the item position. TSD uses its stable sparse-storage position and reserves
`key_set_item_id` for its key set. Item enumeration returns current collection
items. Packed output storage also provides each item's storage layout.

TSD input can enumerate keys, item IDs, values, key-value pairs, or ID-value
pairs. Its [change selections](../../user/reference/input-views.md#dictionary-change-selections)
apply to keys, values, and key-value pairs. `for_each_removed_physical_id` enumerates untagged storage IDs for
removed entries. For a sampled TSD, these identify entries in the previous
dictionary; `for_each_removed` provides their keys. A sampled TSS or TSD input
can also report whether a logical item present in both the previous and current
collections changed physical item ID, then enumerate those `(old_id, new_id)`
pairs. These IDs are unflagged physical IDs; the key for a TSD remapping is
available with `key_at(new_id)`. Ordinary collection inputs report no
remappings.

The [TSD layout](tsd-layout.md) defines dictionary storage and reference
representations.

## Retaining ticks

`TimeSeriesTickWriter::write(input, now)` stores one value update, invalidation,
or collection delta in a buffer record. The caller supplies the current
evaluation time; the writer records a change only when its `last_updated()`
matches that time. This prevents an older invalidation from being recorded
again when another input causes evaluation. Records contain deltas only, not
snapshots. Replay applies each delta to the existing output and does not clear
a valid collection. Reconstructing a value therefore requires replaying from a
matching baseline, not from an arbitrary later record. Scalar payloads are
owned by the record until replay or discard.

Each record begins with a one-byte header that identifies the update and its
validity. Invalidation has no payload. A valid collection record contains
entries followed by a standalone `end` header. The first valid tick of an empty
TSS or TSD has a valid root header and an empty collection body. Replay marks an
invalid output valid for this event; it leaves an already-valid output unchanged.

The built-in writer supports value, fixed-list, bundle, set, and dictionary time
series, including invalidation. It does not serialize reference bindings.
Provider-defined time series register `TimeSeriesTickOperations` with
`TimeSeriesDispatch<TimeSeriesTickOperations>::instance()`, declared in
`<bg/time_series/tick_operations.hpp>`. This registry is
shared by tick writers, readers, and feedback nodes in the process. Complete
registration before concurrent writes, replay, or graph evaluation;
registration is not synchronized. A dynamically loaded provider module must
remain loaded until process exit because the registry retains its handlers.

Each operations record can supply writing, replay, and discard independently.
Register a record under either a semantic type or a runtime representation.
Writing and replay select exact runtime handlers first, then use the semantic
record when that action is absent from the runtime record. Discard uses the
semantic record because the semantic type determines the record grammar.
Write handlers receive the provider's `In<Series>` view and a restricted
`ScalarBufferWriteContext`. Replay handlers receive a typed output view;
discard handlers receive `util::buffer_reader`. A semantic replay handler must
support every runtime representation that has no exact replay handler.

Discard handlers must destroy provider-owned objects. Failed writes discard
the complete prefix through that same record grammar, so handlers must write
records whose completed prefix can be discarded. All runtime representations
of one semantic time series type must use the same write and discard grammar.

Tick serialization uses the shared `ScalarBufferOperations` for provider
scalars; its registration API is documented in the [scalar API](scalar-api.md#optional-operations).
Built-in scalar types keep their compact encodings. An extension scalar type
without registered write operations uses the owning `ScalarValue<>` fallback.
Each retained record is read and discarded according to its declared semantic
scalar type.

Discard handlers must consume valid records without throwing. The reader
discards unread records during destruction and terminates if that cleanup
fails.

Each writer buffer contains records for one semantic time-series type. Construct
its reader with that same type; individual records do not repeat the type header.

The buffer does not destroy owned tick payloads when its final handle is
released. Create a `TimeSeriesTickReader` for a buffer containing records. It
discards unread records on destruction, releasing their scalar payloads.

## Tick formatting

`<bg/time_series/tick_format.hpp>` provides fmt formatters for live inputs and
retained records. `fmt::format("{}", tick(input, now))` formats the root input
only when it changed at `now`; otherwise it writes `SKIP`. Formatting a retained
record with `tick(type, reader)` reads one complete record from a fork and leaves
the supplied reader and encoded scalar payloads unchanged. The overload
`tick(type, valid, reader)` formats a payload whose root header was already
consumed, as in `GraphRunResult::visit_tick`.
The fmt wrappers borrow live inputs or retained readers and accept no format
specifiers.

Scalar values use the scalar formatter in `<bg/scalar/format.hpp>`. Collection
ticks show changed entries only; invalid children print `INVALID`, dictionary
removals print `REMOVE`, and set removals print `REMOVED(value)`. Live and
retained reference ticks are unsupported. The formatter writes directly to
fmt's format context.

Provider-defined time-series types register `TimeSeriesTickFormatOperations`
with `TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance()`. A live
callback receives a borrowed input. A retained callback receives a reader at
the start of that type's payload and must advance past exactly one payload
without destroying its encoded values. Keep a `TimeSeriesTickReader` alive for
each buffer that contains owned tick payloads. Its destructor discards unread
records.

Live dispatch checks an exact runtime registration first and then the semantic
type registration. Retained dispatch uses the semantic type. Complete
time-series operation registration before concurrent formatting begins.
Callback arguments are borrowed for the call duration; do not retain them. A
provider module must remain loaded while its registered callbacks can run.
Scalar registrations and representations are documented in the [scalar
API](scalar-api.md#scalar-formatting).

## Queue messages

`bg::Queue::write(literal)` returns whether it committed a message. Root `SKIP`
and writes after closure return `false`. Closed writes do not project or serialize
the literal. Serialization errors propagate while the queue is open.

Copying a `Queue` creates an independent writer for the same destination. Use
distinct copies on distinct threads. Do not write through a handle while copying
or using that same handle on another thread. Default-constructed and moved-from
handles reject writes.

The engine owns `bg::details::QueueControl`. Its `close()` rejects new writes and
waits for accepted writes to finish. Destroying the controller closes it. After
closure, the reader discards unread messages before their runtime types are
destroyed. Retained handles remain safe to copy, move, destroy, or write through.
The controller does not destroy encoded payloads.

`RuntimeGraph::make_queue(target)` creates a handle for an outer graph node.
The node template declares `uses_queue` and `uses_output_runtime_type` so the
compiled graph reserves queue storage and retains the target's runtime type.
`CompiledNode::queue_mode()` supplies the target node's configured mode.
Queue outputs and every contained time series must use the `ordinary` guarantee.
The graph creates one shared stream on first use. Handle creation requires a
started graph that has not finalized. After stop, it returns a closed handle.
Inner graphs reject it.
[Graph shutdown](evaluation.md#stop) defines closure and unread-message cleanup.

Queue handles allocate scalar payloads through `std::pmr::new_delete_resource()`.
The concurrent buffer's allocator must support concurrent allocation and
deallocation and outlive every retained handle. Each handle retains its writer
block until destruction; the controller does not retain a root buffer handle.

`bg::details::QueueMessageWriter` is an engine-only writer. It serializes a
`TimeSeriesLiteral` for a destination node into a concurrent buffer. Each frame
starts with a typed `QueueMessageHeader` containing a 32-bit destination node
ID and a 32-bit total frame size, followed by one tick record using the grammar
above. The header uses `alignof(std::max_align_t)`, and the frame size is rounded
to that alignment. The size includes header padding and trailing padding; it
excludes alignment before the header.
Destination node IDs must be smaller than `UINT32_MAX`. Root `SKIP` writes no frame.

If serialization fails after reserving a frame, the writer destroys completed
typed payloads, commits the frame with destination `UINT32_MAX`, and
rethrows the error. Consumers skip a sentinel frame using its size without
parsing a tick payload.

Frame consumers align to the header, use its size to bound the message, and
read or discard the tick payload with the semantic time-series type. They must
not construct a `TimeSeriesTickReader` over the framed buffer.

`bg::details::QueueMessageReader` returns the next non-sentinel target without
consuming that frame. Standard and batch apply leave a conflicting frame
pending, and inspection exceptions leave it pending. Successful apply and typed discard
consume the entire frame, including trailing padding. Replay exceptions consume
the frame. Call `discard(type, mode)` for frames that will not be applied;
the mode selects the scalar payload type. Releasing the reader does
not destroy unread payloads.

Queue message serialization supports `TS`, fixed-size `TSL`, `TSB`, `TSS`, and
`TSD` literals. It rejects references, sparse `TSL`, and provider-defined
time-series types. Custom scalar encodings require a projection handler on the
selected write registration and semantic read and discard handlers. Projection
uses the [scalar buffer operation contract](scalar-api.md#optional-operations).
Missing handlers fail before reservation.

`bg::details::QueueTickApply::apply(output)` reads one tick payload from the
current reader position. In standard mode, it returns `false` when an effective
change targets a time series already changed at the current evaluation time.
Inspection stops at the first conflict. The entire payload remains pending:
the reader position, output, and encoded objects stay unchanged. Fixed-list and
bundle items are checked independently.
Eliding mode applies successive changes in order.

Batch mode requires `TS<List<T>>` leaves, directly or inside fixed-size `TSL`,
`TSB`, and `TSD` outputs. Each written leaf contains one `T`, using the existing
tick grammar with the list's element encoding. The first value for a leaf in
an outer cycle replaces its previous list with a one-element list. Later values
append in queue order. Leaves accumulate independently, and an unchanged leaf
retains its previous list. Subscribers observe the completed lists through
[ordinary queue publication](evaluation.md#queue-input).

An effective invalidation and accumulated values for the same time series use
separate cycles. TSD item updates and removals use the same per-key separation
as standard mode below. A conflicting message remains pending in its entirety
and stops queue reading.

```cpp
const auto events = bg::queue<bg::TS<bg::List<bg::i64>>>(
    bg::start = [](bg::Queue queue) {
        queue.write(literal::TimeSeriesLiteral(bg::i64{1}));
        queue.write(literal::TimeSeriesLiteral(bg::i64{2}));
    },
    bg::mode = bg::QueueMode::batch);
```

The first outer cycle publishes the list `[1, 2]`.

TSD keys are independent in standard mode. Across messages, an effective item
update and removal of the same key use separate cycles. A removed key cannot
be recreated by a later message in the same cycle. Removing an absent key is a
no-op when the TSD is already valid. Updates to distinct time series under one
key remain independent.

In standard and batch modes, admission checks the complete message against
changes already applied in the cycle. Accepted messages replay all entries in
order, including repeated TSD keys. Replay consumes the payload and moves owning
scalar values into outputs.
No-op values are destroyed locally. Custom scalar encodings use their registered
read and discard handlers for each scalar.

If application fails, replay destroys unread values, consumes the rest
of the message, and rethrows the first error. Earlier output mutations remain
applied; replay provides no rollback.

Successful application returns `true`, including no-op payloads. A no-op leaves
the output and its last updated time unchanged; it does not use a standard-mode
cycle slot. An ordinary equal-value assignment publishes a change. A
deduplicated equal-value assignment is a no-op.
