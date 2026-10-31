# Plain node parameter resolution

Status: accepted

## Context

Node authors need plain value parameters to accept scalars and time series
without defining separate implementations. Aggregate lifecycle functions can
run while a required time series has no value.

## Decision

Resolve each plain parameter independently as a scalar or time series input.
Use the existing union resolution and compiled parameter bindings. Explicit
`Scalar<>` and `In<>` declarations retain their category constraints.

Reject user start, stop, and finalization functions when an aggregate plain
field resolves as a time series. Invalidation functions receive placeholders
for those fields and must not read them. Automatic cleanup remains supported.

## Consequences

Mixed declarations keep the same plain-value access during evaluation. Node
authors use explicit views when they need time series access during lifecycle
functions. See the [node API](../design/node-api.md#plain-c-parameters) for the
declaration and scheduling contracts.
