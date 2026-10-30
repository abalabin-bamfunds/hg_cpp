# Invocation-end callbacks

Status: accepted

## Context

Frame extensions may need to finish maintenance after a node mutates output and
before downstream nodes observe the published change. That work must remain
within the output-writing invocation and use its scratch lifetime.

## Decision

An extension registers invocation-end work through an owner object. The runtime
stores callback records in invocation scratch and runs them before publishing
output changes. Each owner has at most one pending callback; dispatch clears
that registration before invoking the callback. Section contexts share their
node invocation's callback scope and scratch. Nested node invocations have
separate scopes.

Root aggregate `OutputState` is held directly on the invocation stack and
borrowed by output fields. Item views continue to use invocation scratch.

See [evaluation context callbacks](../design/evaluation.md#evaluation-context) for the
API contract and lifecycle.
