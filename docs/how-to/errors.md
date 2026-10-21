# Diagnose graph errors

To find a wiring failure, start with the innermost failed operation and its
reported argument types. Wiring errors occur while constructing or completing
the graph; evaluation errors occur when node code processes values.

## Type and overload failures

| Failure | Check | Correction |
| --- | --- | --- |
| No compatible numeric overload | Operand scalar types | [Convert](conversion.md) deliberately or use matching literals |
| Optional argument rejected | Receiver's validity constraint | Declare an optional parameter or supply a fallback |
| Native truth conversion of a connection | `if`, Python `and/or/not`, chained comparison | Use [graph conditionals](../guide/conditions.md) |
| Missing collection output type | Generic or untyped signature | Specialize the graph or supply the required result type |
| Missing service implementation | Catalogue and scalar filters | Register one matching implementation |
| Dependency cycle | Connections or mesh dependencies | Remove the same-cycle cycle or use the documented feedback facility |

Python exceptions retain their original type, identity, and traceback where
propagated through graph callbacks. Exception notes add graph-call context.
C++ `WiringError` provides diagnostic and context information; preserve the
underlying cause when reporting it. Read candidate rejection reasons before
changing a signature.

## Evaluation failures

An invalid required input normally prevents evaluation. Reading an invalid
optional input without checking validity is a callback error. Division by zero,
unsupported numeric conversions, and exceptions in user callbacks propagate
from the runner after graph cleanup.

Use a small `test_graph` reproduction with the failing inputs, including their
order and skipped positions. Keep invalidation distinct from no update. Check
[view lifetime](../reference/input-views.md) if an error appears after a callback.
Python lazy views expire; materialize owned data before retaining it.

## Missing or unexpected updates

- Confirm that a value has become available. Required inputs gate calculations.
- Check whether the input updated or merely retained its current value.
- Check deduplication, selected inputs, filter conditions, sample signals, and timers.
- Check whether a collection change affects the subscribed key or item.
- Check the run's exclusive stop boundary and whether its source is finite.

[Testing](testing.md) records these cases without application sources. A valid
current value can remain present even when no output update is recorded.
