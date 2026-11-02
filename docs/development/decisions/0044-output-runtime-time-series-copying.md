# Output-runtime time series copying

Status: accepted

## Context

A copied input and its output can use different runtime representations.
Generic copying must update the selected output representation and publish
through its normal mutation operations.

## Decision

Put generic `copy_tick()` and `copy()` dispatch on the output runtime type.
Built-in runtime types implement copying through their input and output views.
Fully typed value outputs retain direct value access.

## Consequences

Providers implement copying for their output representations through the same
runtime interface. Dispatch adds no fields to runtime objects.

See the [copying API](../design/node-api.md#output-parameters) and
[provider contract](../design/time-series-providers.md#runtime-types).
