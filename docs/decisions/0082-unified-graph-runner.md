# Unified graph execution and output capture

Status: accepted

## Context

Graph execution and test execution require the same wiring, compilation, and
runtime lifecycle. Initial values and update sequences are also useful outside
tests. A separate test runner duplicated execution behavior and restricted
capture to simulation.

C++ graph functions can return wiring points whose concrete type is known only
after wiring their bodies.

## Decision

Use `run_graph` for execution and testing. Resolve top-level parameter types
before interpreting arguments. A matching value takes precedence over an
update sequence; `ticks(sequence)` selects updates explicitly. Scalar
parameters retain normal wiring-time binding.

Resolve C++ graph-function inputs through normal graph invocation, then infer
the output from the returned wiring point. This preserves argument validation,
defaults, requirements, and graph context scopes.

Capture returned output by default in either evaluation engine. Capture stores
only timestamped output events. Dense microsecond indexing and test comparisons
read those events lazily. Disabling capture omits its sink and storage.

See [direct execution](../graph-wiring.md#direct-execution) and
[result access](../testing.md#test-layout).

## Consequences

Initial values, update sequences, and output capture share execution services,
profiling, run limits, cancellation, and cleanup. Long gaps between output
updates require storage proportional to the captured events.
