# Optional wiring arguments

Status: accepted

## Context

Operators can define different behavior for invalid inputs. A fallback value
depends on the operation: addition can use zero while multiplication can use
one. Runtime validity can change after wiring, so it cannot select an overload.

## Decision

Use `opt(point)` to require an optional receiving parameter during wiring.
Preserve the qualifier through graph inputs and variadic arguments. For
unmarked arguments, prefer required parameters when parameter types are
equivalent. Include qualifiers in overload-resolution cache keys.

## Consequences

The selected implementation owns invalid-input behavior. The qualifier adds
no runtime node or storage and supplies no fallback value. `default_` remains
the explicit fallback operation.

See [operators](../operators.md#optional-arguments) and
[graph wiring](../graph-wiring.md#wiring-points).
