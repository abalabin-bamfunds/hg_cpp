# Reduce

`reduce(graph, values, "default"_n = ...)` combines the valid values in a TSD
into one time series. The reducer graph has two explicit time-series inputs
and returns a scalar-valued time series compatible with the TSD item type.
Context inputs are allowed. A generic C++ lambda can be used when the TSD
argument has a statically known item type.

Reducer inputs and the reduce output use the ordinary guarantee. A tree
position can receive a different item after the TSD changes, regardless of the
guarantee on an individual item.

The reducer operation must be associative and commutative. TSD item order is
not part of the reduce contract.

A single valid item is forwarded directly without creating a reducer graph
instance.

Invalid items are omitted. The default scalar is used when the TSD is valid but
has no valid items, including when keys exist but all their items are invalid.
An invalid TSD produces an invalid result. Without a default, a valid TSD with
no valid items also produces an invalid result.

When a TSD stores references, target updates, invalidations, and revalidations
update the reduction even when the TSD membership and stored references do not
change.

Reduce selects its execution path during wiring. It uses compact evaluation
when the reducer supports borrowed evaluation and both compiled reducer input
runtime-type pointers and its output runtime-type pointer exactly match the
source TSD item's runtime-type pointer. Final-guarantee items therefore use
nested evaluation with the ordinary-guarantee reducer graph. A reference-wrapped
TSD also uses nested evaluation when sampled items can have different runtime
types. Compact evaluation stores valid items in a compressed binary Patricia
tree keyed by their physical TSD item IDs. Item changes update affected paths;
membership changes and physical-ID remapping update the tree structure.

Each reducer instance passes two `RuntimeGraphInput` records to its graph
evaluation. Its pair state retains input validity and timestamps, but no input
value pointers. The source values remain owned by the TSD or another reducer
instance for the duration of that evaluation.

Other reducer graphs use nested runtime graphs arranged in adjacent pairs by
TSD iteration order. A membership change rebuilds that graph set. Stopped
nested graphs remain allocated until the next outer cycle so reference removal
completes before their storage is released.

The compact-storage check may call the reducer graph function a second time
during wiring, using an independent graph wiring. If that call produces a
different graph body, reduce uses nested runtime graphs. Wiring-time side
effects in the function run on both calls.
