# WebSocket runtime state

Status: accepted

## Context

WebSocket connections outlive their HTTP upgrade requests and carry message
updates, route state, and authentication state independently.

## Decision

Keep incoming WebSocket messages on a dedicated batched queue. Combine each
message dictionary with the connection request dictionary using the request
keys, so handlers receive connection metadata and messages under the same
connection IDs.

When removing a route, remove it from route lookup first and close its active
connections with code 1001. Enqueue message-dictionary removal before connection
metadata removal so the final message batch is evaluated before the connection
is removed. This prevents new handshakes from selecting the route as its
existing connections are being closed.

For authenticated routes, close an accepted connection with code 1008 when its
identity becomes invalid or its policy later rejects it.

## Consequences

Separate push-node targets let connection metadata use standard queue mode and
message leaves use batch queue mode. Route removal and authentication revocation
invalidate handler state and close the corresponding transport connections.

See [WebSocket server](../../../extensions/web/README.md#websocket-server) for
the API and lifecycle.
