# Library integer division

Status: superseded by [native floating-point integer division](0060-native-integer-division.md).

## Context

Converting integer operands to `double` before division can change the rounded
quotient. For example, `9007199254740993 / 3` must produce
`3002399751580331.0`, matching Python integer true division.

## Decision

Round the exact integer quotient to the nearest `double`, with ties-to-even.
Use hardware division when both operands have exact `double` representations
within the consecutive-integer range. Otherwise, compute the significand and
rounding decision using unsigned integer arithmetic.

## Consequences

The C++ and Python operators share one implementation without a Python runtime
dependency or platform-specific wider integer type.

See [scalar operators](../design/library.md#scalar-operators) for the public contract.
