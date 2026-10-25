# Output publication

A node output retains its current value until a later publication or invalidation.
Publication schedules downstream consumers according to their subscriptions and input
constraints.

## Returned values

Python nodes publish by returning a scalar or collection delta. `None` leaves the output
unchanged; `INVALID` invalidates supported types. C++ callable nodes publish returned
scalars; a disengaged `std::optional<T>` leaves output unchanged. No update preserves
availability as well as the value.

Time series collections publish deltas. Missing fields, positions, or keys in a delta
remain unchanged. Dictionary `REMOVE` removes an item. Set removal uses `Removed` in
Python or `REMOVED` in C++ literals. See [literal forms](literals.md).

## Explicit C++ output access

`Out<TS<T>>` provides `set(value)` and `invalidate()`. An aggregate node can store it as
an output field. A callable can take it as a parameter and return `void`. A node has at
most one root output.

Collection outputs expose child outputs: `output[key].set(value)` updates one dictionary
item; `output.erase(key)` removes the key. Bundle and fixed-list outputs expose their
declared children. Copy operations distinguish copying a current complete value from
copying the current tick; choose the operation matching the node's contract.

A deduplicated output suppresses a mutation equal to its current value. Final and const
outputs reject publications that violate their guarantees. These constraints also apply
to contained time series. See [guarantees](types.md#guarantees).

## Previous output in Python

An injected `output: Output = None` exposes the previous output through the read-only
input-view API. It does not supply setters; publication still uses the returned delta.
Its `modified` and `delta_value` refer to the current cycle and are normally false and
absent before the callback returns.

Output views expire after the callback. Generators cannot request `Output` because their
frames survive between callbacks.
