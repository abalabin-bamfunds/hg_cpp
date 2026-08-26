# Time series references

Status: working design. Open choices are listed at the end.

## Purpose

A time series reference lets a node route a time series without reading or
copying its value.

This document uses `TS_T` for any one time series type and `REF<TS_T>` for a
reference to that type. For example, `REF<TS<T>>` refers to a time series value.

## Wiring

The wiring layer selects one of these actions:

| Binding | Action |
| --- | --- |
| Equal types | Use the output directly |
| `REF<TS_T>` to its target `TS_T` | Use a runtime target pointer and sampling state |
| Other compatible types | Insert or reuse a node that provides the requested representation |

Wiring selects representations transparently. Node code receives the type
declared by its node signature.

The inserted node belongs to the graph that owns the source. Wiring reuses one
such node for each source output and records each representation requested by
the complete wired graph.

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
the data needed to update the receiving input. Each runtime graph stores the
mutable target pointer and sampling state used by that input.

## Subscriptions

Direct wired subscriptions are stored in compiled arrays and shared by runtime
graphs with the same wiring.

When graph compilation inserts a node that provides representations, it adds a
subscription entry for that node to the source output's compiled subscription
array. The entry contains the runtime offset of that node's state.

The source runtime graph owns the created representations and their subscriber
arrays. Each subscriber buffer is allocated when its first subscriber is added.
Subscriber records are contiguous. Each record identifies the destination
runtime graph and the nodes to schedule.

When the source changes, scheduling processes this entry and updates the created
representations affected by the change. It then schedules their subscribers.

When a reference changes target:

1. Remove an active receiving input's subscription from the old target.
2. Update its runtime target pointer.
3. Add an active receiving input's subscription to the new target.
4. Sample the new target.
5. Schedule the receiving node when the input is active.

For the receiving input, the reference update time becomes the sampled view's
last updated time. The target retains its own last updated time. Later target
updates schedule the destination directly.

The [validity design](time-series-validity.md#references) defines reference
invalidation, target invalidation, and target removal.

## Nested graphs

An inner graph input can receive a reference to an outer-graph output. The
boundary subscription schedules the affected inner nodes directly.

An inner graph can return a reference to an inner or outer time series. Outer
consumers subscribe directly to the target. Passing an input reference through
an inner graph copies the target pointer and removes the inner graph from the
value-update path.

An inner graph failure invalidates its reference output and unbinds its
subscribers. The [evaluation design](evaluation.md#exceptions) defines the
complete failure sequence.

The engine creates boundary subscriptions for each inner runtime graph. Runtime
creation is required because wiring cannot determine their number.

When an outer consumer requests a compatible type that the inner graph did not
request during compilation, the runtime graph that owns the source creates or
returns the shared representation for that type.

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
4. They reclaim the storage after removal work is complete.

Removal notification applies to dynamic time series and the outputs of removable
inner runtime graphs.

References to live time series can persist across outer cycles. References to a
removed time series must be cleared during the cycle in which it is removed.

## Outer-cycle boundary

An outer cycle ends after all outer and inner work has finished, including work
that runs concurrently. The engine reclaims removed storage after this point.

## Open implementation choices

1. Representation of reference time series state.
2. Creation, layout, lifetime, and removal of dynamically created
   representations.
3. Layout and removal method for compact subscriber records.
4. Storage for receiving inputs created at runtime.
5. How a time series dictionary locates an item's optional subscriber array.
6. Synchronization of subscription and representation changes between runtime
   graphs.
