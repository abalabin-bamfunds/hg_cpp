# Reduce

`reduce(graph, values, "default"_n = ...)` combines the valid values in a TSD
into one time series. The reducer graph has two explicit time-series inputs
and returns a scalar-valued time series compatible with the TSD item type.
Context inputs are allowed. A generic C++ lambda can be used when the TSD
argument has a statically known item type.

Reducer inputs and the reduce output use the ordinary guarantee. A tree
position can receive a different item after the TSD changes, regardless of the
guarantee on an individual item.

The reducer applies to pairs of values in a balanced tree. An unpaired value
advances to the next level unchanged. A single valid item is forwarded without
creating a reducer graph instance.

Invalid items are omitted. The default scalar is used when the TSD is valid but
has no valid items, including when keys exist but all their items are invalid.
An invalid TSD produces an invalid result. Without a default, a valid TSD with
no valid items also produces an invalid result.

When a TSD stores references, target updates, invalidations, and revalidations
update the reduction even when the TSD membership and stored references do not
change.

Reducer graphs that support borrowed evaluation use per-pair reducer storage
when the runtime representations are compatible. Other reducer graphs use
nested runtime graphs. A change to the key set or to the reference stored under
a key rebuilds the tree. Stopped nested graphs remain allocated until the next
outer cycle so reference removal completes before their storage is released.

The compact-storage check may call the reducer graph function a second time
during wiring, using an independent graph wiring. If that call produces a
different graph body, reduce uses nested runtime graphs. Wiring-time side
effects in the function run on both calls.
