# Native floating-point integer division

Status: accepted

## Context

Library integer division needs a small implementation with predictable execution
cost. Its result type is `double`.

## Decision

Convert both `i64` operands to `double` and divide using the native C++ operation.
Keep the zero-divisor check. Define the complete operation in its overload
registration lambda.

## Consequences

Large integer operands can lose precision during conversion. C++ and Python graph
calls use the same implementation, whose results can differ from Python integer
true division.

See [scalar operators](../design/library.md#scalar-operators) for the public contract.
