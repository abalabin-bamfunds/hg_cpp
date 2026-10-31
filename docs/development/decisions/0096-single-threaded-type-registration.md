# Single-threaded type registration

Status: accepted

## Decision

Register semantic and runtime descriptors on one thread during wiring,
compilation, or evaluation. Node code can discover a concrete scalar subtype
when constructing a value during evaluation. Registration failures propagate
and abort the current operation. Continued registry use after a registration
failure is unsupported. Completed descriptors support concurrent reads.

Recursive construction registers a stable descriptor before resolving its
children. Completion connects children, finalizes derived properties, and
validates the result. Registration retains descriptors for the process lifetime.

The canonical semantic factory records a lazy callback for its default runtime.
C++ factories obtain that runtime from the declaration's storage or time series
traits. Recording the callback does not construct the runtime. Literal input
descriptors retain semantic types and resolve their defaults during wiring.

## Consequences

Registration does not require transactions, rollback, retry, atomic publication,
or locks. Recursive capability propagation and cycle validation remain required.

The [type system](../design/type-system.md#type-model) defines registration and
descriptor ownership. This supersedes
[atomic recursive semantic registration](0018-atomic-recursive-semantic-registration.md).
