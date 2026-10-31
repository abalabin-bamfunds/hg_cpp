# Library integer arithmetic

Status: accepted

## Context

Native signed overflow has undefined behavior. Library integer calculations
need defined results without per-evaluation overflow checks.

## Decision

Perform integer arithmetic in the corresponding unsigned type and convert the
result back using the C++23 signed conversion rules. Apply the same arithmetic
to integer-backed timestamps and durations.

## Consequences

Arithmetic wraps at the underlying integer width. Negation and absolute value
of the minimum integer preserve that value. These operations introduce no
overflow error path.

See [scalar operators](../design/library.md#scalar-operators).
