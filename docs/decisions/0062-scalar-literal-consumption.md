# Scalar consumers own literal interpretation

Status: accepted

## Decision

The scalar layer owns literal construction and replacement. `ScalarValue::from_literal`
and scalar time series updates reuse one recursive DDV construction consumer.
Literal carriers provide source visitation; runtime type APIs receive source-neutral
construction callbacks. See the [literal API](../scalar-api.md#literals).

Scalar replacement preserves values retained by owning downstream copies. Native
and composed list replacements construct separate item storage and install it by
swapping storage headers. See the [replacement contract](../node-api.md#output-parameters).

## Reason

One scalar consumer keeps syntax interpretation independent of time series
publication and reuses direct construction without a second collection copy.
Separate replacement storage preserves previously published scalar values.
