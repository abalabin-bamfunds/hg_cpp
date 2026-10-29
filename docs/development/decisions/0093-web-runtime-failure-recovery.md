# Web runtime failure recovery

Status: accepted

## Context

HTTP and WebSocket transports run outside graph evaluation. A callback or
transport failure must not stop unrelated requests and connections.

## Decision

Log runtime failures through the logger. Do not expose a runtime error time
series. Catch failures at request, connection, command, and completion
boundaries, then fail or close only the affected entry. Keep bind and listen
failures as startup errors.

When libcurl reports a multi-engine failure, return status 599 for every active
request, discard the old multi handle while its transfers remain alive, and
create a replacement. Do not replay requests. Bound retry waits and synchronize
wakeup with handle replacement. Reject duplicate route registrations locally
and retain the existing route.

## Consequences

Transport loops continue after recoverable runtime failures. HTTP client
callers receive the existing status-599 response for failed transfers.

See the [web extension](../../../extensions/web/README.md) for API behavior.
