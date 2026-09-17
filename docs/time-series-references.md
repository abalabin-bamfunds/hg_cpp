# Time series references

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

`Reference` is one word that identifies a root time series, a contained item,
or a representation. Passing a reference copies that word. Resolving a
reference provides the selected time series storage, runtime type, and
last-updated time. A root reference uses its root directly. Collection
representations and item references create provider data only when requested.

An item reference retains its parent reference and item ID. A representation
reference retains its base reference and representation index. Operations follow
these references to the root and obtain provider data there. This preserves
provenance through nested collections without a provider pointer in each
reference.

For a source reached through a reference, the receiving binding sends its type
request to the node associated with the target. The graph that owns the target
creates the representation.

## Dynamic representations

The inserted node owns dynamically created time series. This is required because
a reference from a nested graph can receive a compatible type request after the
target graph has been compiled.

An exact-type request returns the source time series. A repeated request returns
the existing representation for the source and requested type. A new compatible
request allocates its representation through the runtime graph that owns the
source. Runtime storage is proportional to the representations that are used.

The same process applies recursively to collection time series. Examples include:

| Source | Requested representation |
| --- | --- |
| `TSD<K, TS_T>` | `TSD<K, REF<TS_T>>` |
| `TSD<K, REF<TS_T>>` | `TSD<K, TS_T>` |

A `TSD<K, REF<TS_T>>` representation of `TSD<K, TS_T>` shares the source key
storage and item IDs. Its item references identify the source items. Converting
that representation back to `TSD<K, TS_T>` returns the source dictionary.
Source item updates notify consumers of the item references without updating
the reference dictionary itself.

A `TSD<K, REF<TS_T>>` constructed by a node has no source dictionary.
Converting it to `TSD<K, TS_T>` creates a representation that retains every
key and exposes the current reference target as its item. An empty reference
produces an invalid item. Target changes and target updates modify the
corresponding item without evaluating an intermediate node.

When `REF<TS_T>` is bound to its exact target type, compiled-node data records
the reference output's last updated location. Each runtime graph stores the
current reference used by the receiving input.

## Subscriptions

Direct wired subscriptions are stored in compiled arrays and shared by runtime
graphs with the same wiring.

A `REF<TS_T>` to `TS_T` binding creates two subscriptions. The node
subscription applies the input's activity. An output-notification record stores
the receiving node, resolved input-slot offset, activity, validity policy, and
requested target runtime type.

The compiled output metadata associates these records with the REF notification
handler. After the output changes, the runtime invokes the handler once for the
complete record span. The handler updates target pointers and dynamic
subscriptions, then requests required-input validity checks. The runtime does
not interpret REF storage.

The reference subsystem compares the resolved target runtime type with the
requested runtime type. A type-specific representation factory checks contained
runtime types when required. Runtime reference handling does not run semantic
type matching or construct a type map.

The reference-making node receives input changes through a compiled input
notification and notifies its root reference. Each representation observes its
base reference. It consumes that notification, updates its state, and notifies
only the affected representation or item references.

The source runtime graph owns the created representations and subscriber
groups. Ordinary subscriber records identify the destination runtime graph,
node, and input slot.

A TSD item reference records item notifications for destination representations.
Each notification stores a destination-table index and destination item ID. The
shared destination entry stores the graph, representation, and notification
functions. The first destination item is stored with the item reference. Fan-out
uses root-owned destination-item groups. A TSD batches changed item references.
Each destination representation publishes once with its changed item IDs.
Notification work is proportional to changed item references and their destinations.

The reference provider owns references, subscriber groups, item notifications,
and type-specific components. TSD and fixed TSL item selection each use a
lazily created component.

A reference notification notifies its dynamic subscribers directly. Subscriber
records retain the destination graph. A runtime observer uses that graph's
context and the notification's outer-cycle time.

When a reference changes target:

1. Remove any receiving input subscription from the old target.
2. Store the new reference.
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

### Input sampling

A dereferenced input distinguishes an ordinary target update from a change of
target. Binding its first target samples that target as a first update. Removing
the target reports the previous collection as removed.

Switching from collection `A` to collection `B` reports the transition that the
node observes:

- TSS and TSD additions are the keys or items in `B` but not `A`.
- TSS and TSD removals are the keys or items in `A` but not `B`.
- A TSD item present in both is modified unless both entries identify the same
  item time series.
- A contained collection applies the same rules recursively.

An initial sample reports every current key or item as added. A removed sample
reports every previous key or item as removed. Sampling is change information
for the current evaluation cycle. Every input sees the same sample. A passive
input does not replay the sample in a later cycle.

The representation retains the previous target until old change information is
cleared. A reference output publishes after its node finishes evaluating, so it
publishes at most one target change per cycle.

An input view reports whether its visible last-updated time is the current
evaluation time without requiring evaluation-context access. Sampling reports
the sampling time as the visible last-updated time.

The [validity design](time-series-validity.md#references) defines reference
invalidation, target invalidation, and target removal.

## Nested graph boundaries

An inner graph input can receive a reference to an outer-graph output. The
owning node applies changes to the boundary reference. Target updates schedule
the affected inner nodes through their dynamic subscriptions.

An inner graph can return a reference to an inner or outer time series. Outer
consumers subscribe directly to the target. Passing an input reference through
an inner graph copies the reference and removes the inner graph from the
value-update path.

When an outer consumer requests a compatible type that the inner graph did not
request during compilation, the runtime graph that owns the source creates or
returns the shared representation for that type.

The [nested graph design](nested-graphs.md) defines boundary isolation,
scheduling, and lifecycle.

## Dynamic collections

TSD representations use the shared-key structure defined by the
[TSD layout](tsd-layout.md#shared-representations).

A fixed TSL item selection uses its stable item ID. A runtime index creates
selection state only for the selected item. An invalid, negative, or
out-of-range index produces an invalid reference. A value item produces an item
reference. A reference item forwards its stored reference. Updates notify only
selections for the changed items.

When an item is first used through a reference, its item reference receives a
subscriber group from the provider that owns its root. Item updates notify that
item reference directly.

A TSD creates item-reference storage only when an item is selected through a
reference. Selection retains the key's sparse item ID and creates a non-live key
when the key is absent. The key set reclaims an unretained non-live ID after the
outer cycle in which it was released.

Selecting an item returns an `In<REF<V>>` that uses the canonical `REF<V>`
runtime type. The input carries a separate `ReferenceBinding`; runtime types do
not own selection state or subscriptions. A dynamic input uses the binding to
subscribe to selection changes.

A value item resolves to its item reference. A reference item resolves to its
stored reference without creating `REF<REF<V>>`. Replacement or removal of the
selected item schedules the selecting node. Updates from a stored reference's
target bypass that node.

The selection owns the selected reference. An unused selection remains alive
until the next outer-cycle callback. This preserves temporary selections for
their evaluation and lets removal reclaim only entries that remain unused.

The shared set and its representations handle key removal:

1. The set marks the key and sparse index as removed.
2. Each representation notifies the affected item's subscribers and nodes that
   received its reference.
3. The set and representations keep the key and item storage valid until the
   outer cycle ends.
4. The release callback removes dependent representations and reclaims the
   item reference after its subscriber group is empty.

Removal notification applies to dynamic time series and the outputs of removable
inner runtime graphs.

References to live time series can persist across outer cycles. A node that
produces a reference value is responsible for replacing or invalidating it when
its selected target changes.

## Outer-cycle boundary

An outer cycle ends after all outer and inner work has finished, including work
that runs concurrently. This is the earliest point at which removed storage can
be reclaimed.
