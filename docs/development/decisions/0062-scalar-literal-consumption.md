# Scalar consumers own literal interpretation

Status: superseded in part by [0066](0066-shared-literal-sentinels.md).
Scalar construction and replacement ownership remain in effect; 0066 replaces
the separate-carrier decision.

## Decision

The scalar layer owns literal construction. Scalar time series updates call
`ScalarValue::from_literal` and consume the resulting owner through `set_value`.
One recursive DDV construction consumer interprets literals. Literal carriers
provide source visitation; runtime type APIs receive source-neutral construction
and storage operations. See the [literal API](../design/scalar-api.md#literals).

Time series deltas use a separate borrowed carrier. Its scalar visitation reuses
this construction consumer; output methods interpret its update markers. See the
[output API](../design/node-api.md#output-parameters).

Scalar replacement preserves values retained by owning downstream copies. Native
and composed collection replacements construct separate item storage and install
it by swapping storage headers. See the [replacement contract](../design/node-api.md#output-parameters).

## Reason

One scalar consumer keeps syntax interpretation independent of time series
publication. Consuming an owning scalar reuses the same storage transfer for
literal and programmatic values. Compatible collection storage transfers without
a second element copy. Separate replacement storage preserves previously
published scalar values.

Separate carriers preserve scalar-only syntax. Shared scalar visitation keeps
one recursive construction implementation for both carriers.
