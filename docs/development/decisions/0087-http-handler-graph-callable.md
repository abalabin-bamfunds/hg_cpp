# HTTP handler graph callable

Status: accepted

## Context

HTTP routes must wire service requests at the call site and preserve the handler's
ambient inputs and output type checks.

## Decision

`http_handler(path, graph)` returns an ordinary C++ lambda that captures the path,
handler definition, and optional policy service. Calling the lambda wires the route
through the HTTP request/reply service. The caller keeps the returned lambda alive
through graph compilation and execution. The lambda owns the handler definition and
refers to the policy service without taking ownership.

The lambda forwards its arguments to the handler wiring operation. Handler signature
resolution and output validation occur when that operation invokes the handler.

## Consequences

Routes do not add a wiring-template signature around the handler. Handler resolvers,
ambient inputs, and additional outputs remain governed by the handler's own graph
signature.

See the [HTTP server extension](../../../extensions/web/README.md) for the public API.
