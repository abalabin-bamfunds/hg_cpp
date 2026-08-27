# Time series validity

Status: working design.

A time series is valid when its last updated time is greater than `MIN_DT`.
Invalidation sets its last updated time to `MIN_DT`.

## Invalidation and revalidation

`ordinary` and `deduplicated` time series support
[invalidation](terminology.md#invalidation). `const` and `final` time series
remain valid after their first update.

Revalidation behaves as a first update. It supplies a current value, records
the update time, and schedules subscribers. A `deduplicated` time series always
publishes revalidation because an invalid time series has no current value for
comparison.

An input view can become invalid while its source time series remains valid.
Source guarantees and input-view validity are therefore separate wiring
properties.

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

Collection invalidation has these effects on an input view:

- `TSS` reports every visible item as removed
- `TSD` reports every visible key as removed
- `TSL` and `TSB` make every item view invalid

Contained time series storage and values remain unchanged. A `valid_items` TSD
view reports only its visible keys.

On revalidation, `TSS` and `TSD` report their visible entries as added. Each
valid `TSL` and `TSB` item behaves as a first update.

## Nodes

A required input view becoming invalid applies the node's invalidation behavior
once. The [node API](node-api.md#required-input-invalidation) defines the
available behavior.

A node with a `const` or `final` output must preserve its output when a required
input can become invalid after the output's first update. Wiring rejects an
incompatible invalidation behavior.
