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
code receives the type declared by its signature. Transparent dereferencing
limits the exposed guarantees according to the
[reference guarantee rules](type-system.md#reference-guarantees).

A node that handles a reference declares `In<REF<TS_T>>` or
`Out<REF<TS_T>>`. A reference output selects or passes through an input
reference. Wiring inserts the required reference-making node for a time series
output.

The inserted node belongs to the graph that owns the output. Wiring reuses one
such node for each output and records each representation requested by the
complete wired graph. [Explicit wiring-point conversion](graph-wiring.md#wiring-points)
retains a deferred reference point until a consumer requires it.

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

The receiving binding sends a representation request to the node associated
with the target. The graph that owns the target creates the representation.

## Dynamic representations

The inserted node owns dynamically created time series. This is required because
a reference from a nested graph can receive a compatible type request after the
target graph has been compiled.

An exact-type request returns the target time series. A repeated request returns
the existing representation for that target and requested type. A new compatible
request allocates its representation through the runtime graph that owns the
target. Runtime storage is proportional to the representations that are used.

TSD representation runtime types have stable lifetime independent of individual
representations. A resolved TSD representation uses the representation object as
its storage; its runtime type resolves the current base TSD when it reads items
or change information.

TSD supports compatible representations recursively. Examples include:

| Provided type | Requested representation |
| --- | --- |
| `TSD<K, TS_T>` | `TSD<K, REF<TS_T>>` |
| `TSD<K, REF<TS_T>>` | `TSD<K, TS_T>` |

A `TSD<K, REF<TS_T>>` representation of `TSD<K, TS_T>` shares the original key
storage and item IDs. Its item references identify the original items.
Converting that representation back to `TSD<K, TS_T>` returns the original TSD.
Item updates notify consumers of the item references without updating the
reference TSD itself.

A `TSD<K, REF<TS_T>>` constructed by a node has no underlying value TSD.
Converting it to `TSD<K, TS_T>` creates a representation that retains every
key and exposes the current reference target as its item. An empty reference
produces an invalid item. Target changes and target updates modify the
corresponding item without evaluating an intermediate node.

Fixed TSL and TSB representations apply the same reference binding rules to
their contained time series. A value parameter observes each reference item's
target, including target updates, changes of target, and invalidation. A
reference parameter receives a stable reference to each value item without
copying its value. Value-item updates notify target subscribers without changing
those reference values. Both use the collection's stable item IDs. A direct
binding that requires no item adaptation creates no representation.

A free TSL or TSB exposes each referenced item's target state. Its item's
last-updated time comes from the target rather than the collection node's
input binding. An active collection sample retains its sampled changes until
cycle cleanup.

Each dereferencing node input stores its current reference in one word. Tag
`11` identifies an allocated two-word block containing the actual reference
and the input's retained update state. Its allocation uses the memory resource
of the runtime graph that owns the stored reference. The binding allocates the
block only when suppressing a same-target reference tick requires preserving
the input's last-updated time. Input resolution restores the direct reference
and frees the block when the target's last-updated time is later than the
reference's current publication time. Replacing the target or destroying the
binding also frees it.

## Subscriptions

Reference subscriptions implement the dependency and publication rules of the
[evaluation model](evaluation.md#evaluation-model).

Direct wired subscriptions are stored in compiled arrays and shared by runtime
graphs with the same wiring.

A `REF<TS_T>` to `TS_T` binding uses an output-notification record that stores
the receiving node, resolved input-slot offset, activity, validity policy, and
requested target runtime type.

The compiled output metadata associates these records with the REF notification
handler. After the output changes, the runtime invokes the handler once for the
complete record span. The handler updates target pointers and dynamic
subscriptions, then applies the input's activity and validity policy to changes
of the dereferenced input. The runtime does not interpret REF storage.

The reference subsystem compares the resolved target runtime type with the
requested runtime type. A type-specific representation factory checks contained
runtime types when required. Runtime reference handling does not run semantic
type matching or construct a type map.

The reference-making node receives input changes through a compiled input
notification and notifies its root reference. Each representation observes its
base reference. It consumes that notification, updates its state, and notifies
only the affected representation or item references.

The runtime graph that owns the referenced output owns its created
representations and subscriber groups. A subscriber identifies a receiving
runtime graph, node, and input slot. It updates the input and applies the
input's activity and validity rules.

Representations own any item references they create for recursive adaptation.
Provider teardown detaches all representation subscriptions before freeing
representation storage. This is required because a child representation can
be created after its parent when a dictionary gains a key.

Root references and collection membership values store their first node
subscriber directly. A second distinct node subscriber requires an allocated
subscriber collection. A root input callback also requires that collection.
Creating a representation promotes the root to a provider, which retains any
existing subscribers.

An observer connects one runtime reference object to another without adding a
node evaluation. Representations and item selections use observers to maintain
their own state before they notify their subscribers.

A TSD item reference uses a compact descriptor for each destination
representation. Shared provider data stores the destination and notification
functions. A TSD batches changed item references, then each destination
representation publishes once with its changed item IDs. Notification work is
proportional to changed item references and their destinations.

The reference provider is created only when a root needs dynamic behavior. It
owns representations, subscriber groups, item-notification groups, and
type-specific components. TSD selection and fixed TSL or TSB item selection
each use one lazily created component per root.

A reference notification notifies its dynamic subscribers directly. Subscriber
records retain the destination graph. A runtime observer uses that graph's
context and the notification's outer-cycle time.

A dynamic input callback runs immediately. Deferred cleanup runs once at outer
cycle entry before any inner graph can publish a change, so callback delivery
does not depend on whether the receiving inner graph has evaluated.

When a reference ticks at a dereferencing binding:

1. Adapt the new target to the receiving input's requested type.
2. Retain the old selected view's validity before the current cycle.
3. Compare the handles. If they differ, compare their resolved storage and runtime type.
4. Store the new handle. Replace the target subscription when the handle differs
   and the input is active, has a `required` or `all_valid` constraint, or
   updates another logical output.
5. Sample the target only when the target changes.

Replacing only the handle preserves the binding's last-updated time.
Target changes and target updates apply the input's activity and validity policy.
An explicit `REF<TS_T>` input receives reference ticks through normal time
series subscriptions; these dereferencing rules do not apply to it.

When the target changes, the reference tick becomes the sampled view's
last-updated time. View construction selects the later of the reference and
target update times, using the retained state when present. The input is valid
when its selected target is valid.
During the switching cycle, its validity transition compares the retained
pre-cycle validity with the current target validity. Later target updates use
the target's own state and schedule the destination directly. Target references
and shared representations do not retain receiving inputs' switching history.

View construction uses the current target's runtime type. A reference
can therefore select between different runtime types of the same time series
type. An update for an inner graph is applied when that graph enters the current
outer cycle.

### Input sampling

A dereferenced value input distinguishes an ordinary target update from a
change of target. Binding a target reports the binding time as the input's
visible last-updated time without modifying the target.

A `TSD<K, TS_T>` representation of `TSD<K, REF<TS_T>>` also samples changes to
each item reference. Binding its first item target samples that target as a
first update. Removing an item target reports its previous collection value as
removed.

A TSS target switch compares current membership with the previous target's
membership before the current cycle. A TSD target switch compares keys.
A TSL or TSB target switch samples corresponding items by position or field.
Collection samples apply sampling recursively to contained collections. Each
sampled item reports the sampling time and the validity transition between its
pre-cycle and current views without changing either source's state.
One reference notification shares a sample among subscribers with the same old
and new references, resolved previous target state, and requested representation.
The sample materializes change data on its first query. Inputs retain it until
the next outer cycle; stopping or finalizing their graph releases it sooner.

Switching a TSD item target from TSD `A` to TSD `B` reports the transition that
the node observes:

- TSD additions are the keys in `B` but not `A`.
- TSD removals are the keys in `A` but not `B`.
- A TSD item present in both is updated unless both entries identify the same
  item time series.
- A contained TSD applies the same rules recursively.

An initial TSD sample reports every current key as added. A removed sample
reports every previous key as removed. Sampling is change information for the
current evaluation cycle. Every input sees the same sample. A passive input does
not replay the sample in a later cycle.

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

The engine uses `REF<REF<TS_T>>` only to transport a changing reference-valued
context or a late-bound service reply across an inherited graph boundary. It is
not a graph-authoring type.
The inner graph's forwarding node subscribes to the outer reference output and
publishes its current `REF<TS_T>` to ordinary consumers.

When an outer consumer requests a compatible type that the inner graph did not
request during compilation, the runtime graph that owns the target creates or
returns the shared representation for that type.

The [nested graph design](nested-graphs.md) defines boundary isolation,
scheduling, and lifecycle.

## Collection item selection

TSD representations use the shared-key structure defined by the
[TSD layout](tsd-layout.md#shared-representations).

A fixed TSL item or TSB field selection uses its stable item ID. A runtime
selection creates state only for the selected item. An invalid, negative, or
out-of-range TSL index produces an invalid reference. A value item produces an
item reference. A reference item forwards its stored reference. Updates notify
only selections for the changed items.

When an item is first used through a reference, its item reference receives a
subscriber group from the provider that owns its root. Item updates notify that
item reference directly.

A TSD key selection creates selection storage only for that key. It retains the
key's sparse item ID and creates a non-live key when the key is absent. The key
set reclaims a non-live ID when its last retention is released. Removed keys
remain available until the outer-cycle boundary.

A key selection belongs to the currently selected TSD. When a `REF<TSD<K, TS_T>>`
changes its target, the selecting node selects the key again and binds its item
input to the new selection. `getitem` performs this re-bind when its reference
input ticks.

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

## Collection membership subscriptions

The provider shares one membership value for each observed collection key and
view. A missing key has a valid `false` value. An invalid collection has an
invalid membership value. The valid-item TSD view also requires the immediate
item to be valid, following the [validity rules](time-series-validity.md#collections).

A dynamically bound value input subscribes to that membership value. Normal
updates visit changed item IDs and notify only affected consumers. Initial
publication, invalidation, and a change of collection can visit all observed
keys. Native key storage retains sparse IDs while they are observed, including
IDs for absent keys.

Binding replacement and graph stop remove the subscription. Unused membership
state remains available until the outer-cycle boundary, like item-selection
state.

## Outer-cycle boundary

Removed storage remains available until all outer and inner work for its outer
cycle has finished. A cleanup callback reclaims it at the beginning of the next
outer cycle, before new input updates. Stopping or destroying the graph performs
the same required cleanup.
