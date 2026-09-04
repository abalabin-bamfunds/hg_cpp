# Time series references

Status: working design. Open choices are listed at the end.

## Purpose

A time series reference lets a node route a time series without reading or
copying its value.

This document uses `TS_T` for any one time series type and `REF<TS_T>` for a
reference to that type. For example, `REF<TS<T>>` refers to a time series value.

## Wiring

The wiring layer selects one of these actions:

| Argument | Parameter | Action |
| --- | --- | --- |
| `TS_T` | `TS_T` | Use the output directly |
| `REF<TS_T>` | `TS_T` | Dereference and sample the target |
| `TS_T` | `REF<TS_T>` | Insert or reuse a reference-making node |
| `REF<TS_T>` | `REF<TS_T>` | Preserve the reference |

Type resolution matches reference arguments through their targets. Wiring
retains the original argument and selects the action after resolution. Node
code receives the type declared by its signature.

A node that handles a reference declares `In<REF<TS_T>>` or
`Out<REF<TS_T>>`. A reference output selects or passes through an input
reference. Wiring inserts the required reference-making node for a time series
source.

The inserted node belongs to the graph that owns the source. Wiring reuses one
such node for each source output and records each representation requested by
the complete wired graph.

A reference value is a pointer to storage owned by the inserted node. That
storage identifies the target time series, its runtime type, and its
dynamic subscribers. Passing a reference copies this pointer.

For a source reached through a reference, the receiving binding sends its type
request to the node associated with the target. The graph that owns the target
creates the representation.

## Dynamic representations

The inserted node owns dynamically created time series. This is required because
a reference from a nested graph can receive compatible type requests after the
target graph has been compiled.

Compiled data contains creation and update information for types requested by
the complete wired graph. The node also provides an interface that accepts a
compatible resolved type at runtime.

The node's runtime state stores a compact array of created representations. Each
entry contains:

- the requested resolved type
- a pointer to the created time series
- its update operation

An exact-type request returns the source time series. A repeated request returns
the existing representation. A new compatible request allocates its time series
through the runtime graph that owns the source and adds it to the array. Runtime
storage is therefore proportional to the representations that are used.

Build every representation from the original source. This provides one update
path for each source and requested type. All consumers of that pair share the
same time series.

The same process applies recursively to collection time series. Examples include:

| Source | Requested representation |
| --- | --- |
| `TSD<K, TS_T>` | `TSD<K, REF<TS_T>>` |
| `TSD<K, REF<TS_T>>` | `TSD<K, TS_T>` |
| `TSD<K, TSD<K1, TS_T>>` | `TSD<K, TSD<K1, REF<TS_T>>>` |

A TSD representation request records whether the receiving input requires
`valid_items`. The node that provides representations applies the guarantee
when it changes the item type.

When `REF<TS_T>` is bound to its exact target type, compiled-node data records
the reference output's last updated location. Each runtime graph stores the
current target identity used by the receiving input.

## Subscriptions

Direct wired subscriptions are stored in compiled arrays and shared by runtime
graphs with the same wiring.

A `REF<TS_T>` to `TS_T` binding creates two subscriptions. The node
subscription applies the input's activity. An output-notification record stores
the receiving node, resolved input-slot offset, activity, and validity policy.

The compiled output metadata associates these records with the REF notification
handler. After the output changes, the runtime invokes the handler once for the
complete record span. The handler updates target pointers and dynamic
subscriptions, then requests required-input validity checks. The runtime does
not interpret REF storage.

When graph compilation inserts a node that provides references or other
representations, it adds a special entry to the source output's compiled
subscription array. The entry contains the runtime-storage offset of its
dynamic subscriber pointer.

The source runtime graph owns the created representations and their subscriber
arrays. Each subscriber buffer is allocated when its first subscriber is added.
Subscriber records are contiguous. Each record identifies the destination
runtime graph, node, and input slot.

When an exact reference target changes, scheduling processes this entry and
schedules the dynamic subscribers directly. Other representations update the
affected representation before scheduling its subscribers.

When a reference changes target:

1. Remove any receiving input subscription from the old target.
2. Update its runtime target pointer.
3. Add a subscription to the new target when the input is active, required, or
   updates another logical output.
4. Sample the new target.

The node subscription schedules the receiving node when the input is active.

For the receiving input, the reference update time becomes the sampled view's
last updated time. View construction selects the later of the reference and
target update times. Target invalidity makes the view invalid. Later target
updates schedule the destination directly.

View construction uses the current target's runtime type. A reference
can therefore select between different runtime types of the same time series
type. An update for an inner graph is applied when that graph enters the current
outer cycle.

The [validity design](time-series-validity.md#references) defines reference
invalidation, target invalidation, and target removal.

## Nested graph boundaries

An inner graph input can receive a reference to an outer-graph output. The
owning node applies changes to the boundary reference. Target updates schedule
the affected inner nodes through their dynamic subscriptions.

An inner graph can return a reference to an inner or outer time series. Outer
consumers subscribe directly to the target. Passing an input reference through
an inner graph copies the target pointer and removes the inner graph from the
value-update path.

When an outer consumer requests a compatible type that the inner graph did not
request during compilation, the runtime graph that owns the source creates or
returns the shared representation for that type.

The [nested graph design](nested-graphs.md) defines boundary isolation,
scheduling, and lifecycle.

## Dynamic collections

Dictionary representations use the shared-key structure defined by the
[TSD layout](tsd-layout.md#shared-representations).

When an item is first used through a reference, its owning representation
creates and owns its dynamic subscriber array. Item updates iterate this array
directly.

The shared set and its representations handle key removal:

1. The set marks the key and sparse index as removed.
2. Each representation notifies the affected item's subscribers and nodes that
   received its reference.
3. The set and representations keep the key and item storage valid until the
   outer cycle ends.
4. They reclaim the storage on the next collection mutation or graph
   destruction.

Removal notification applies to dynamic time series and the outputs of removable
inner runtime graphs.

References to live time series can persist across outer cycles. References to a
removed time series must be cleared during the cycle in which it is removed.

## Outer-cycle boundary

An outer cycle ends after all outer and inner work has finished, including work
that runs concurrently. This is the earliest point at which removed storage can
be reclaimed.

## Open implementation choices

1. Creation, layout, lifetime, and removal of dynamically created
   representations.
2. Storage for receiving inputs created at runtime.
3. How a time series dictionary locates an item's optional subscriber array.
4. Synchronization of subscription and representation changes between runtime
   graphs.
5. Removal of a runtime graph's dynamic subscriptions from target graphs.
6. Whether removed collection storage should be reclaimed at the outer-cycle
   boundary.
