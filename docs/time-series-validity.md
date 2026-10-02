# Time series validity

A time series is valid when its last updated time is greater than `MIN_DT`.
An invalidatable time series becomes invalid when its last updated time is set
to `MIN_DT`.

## Invalidation and revalidation

`ordinary` and `deduplicated` time series types that support
[invalidation](terminology.md#invalidation) can become invalid. `const` and
`final` time series remain valid after their first update.

Revalidation behaves as a first update. It supplies a current value, records
the update time, and schedules subscribers. A `deduplicated` time series always
publishes revalidation because an invalid time series has no current value for
comparison.

An input view can become invalid while its source time series remains valid.
Source guarantees and input-view validity are therefore separate wiring
properties.

`default_(source, fallback)` makes its receiving input valid while `source` is
invalid. Source validity changes publish updates to that input because its
visible value can change in either direction.

## References

When a reference becomes invalid, each receiving binding:

1. removes its subscription from the old target
2. clears its target pointer and sampling state
3. makes its input view invalid
4. applies required-input invalidation behavior or schedules an active optional
   input

The old target retains its state.

When a target becomes invalid, references to it remain valid. Each dereferenced
input view becomes invalid but retains its target pointer and subscription. A
target revalidation then behaves as a first update.

Target removal makes every reference to the target invalid. The receiving
bindings follow the reference invalidation sequence. Target storage remains
available until the outer cycle ends.

A `const` or `final` reference can identify a target whose lifetime covers the
remaining lifetime of the reference.

## Collections

`TSS` and `TSD` start invalid. Their first tick makes them valid, including a
tick that publishes an empty collection. They remain valid after that tick.
`TSD` item validity is independent of TSD validity.

A TSS or TSD input view can become invalid when its reference is removed or
selects an invalid target. An invalid view has no current collection value. A
dereferenced TSD representation reports changes to its item targets through the
[sampling rules](time-series-references.md#input-sampling).

Removed storage follows the
[outer-cycle lifetime](time-series-references.md#outer-cycle-boundary).

`clear()` publishes a valid empty TSS or TSD. A TSS or TSD output preserves its
value when a required input becomes invalid.

Invalidating a TSB makes every item view invalid. Each item behaves as a first
update when it becomes valid again.

A TSL is valid when at least one item is valid. Invalidating a TSL invalidates
its items. The last valid item becoming invalid invalidates the TSL. The first
item becoming valid revalidates it. Each item validity change records that item
as updated.

## Nodes

A required input view becoming invalid applies the node's invalidation behavior
once. The [node API](node-api.md#required-input-invalidation) defines the
available behavior.

A node with a `const` or `final` output must preserve its output when a required
input can become invalid after the output's first update. Wiring rejects an
incompatible invalidation behavior.
