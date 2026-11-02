# HTTP handler wiring

Status: superseded by [0084-http-service-handler-wiring](0084-http-service-handler-wiring.md)

## Context

An HTTP adaptor must keep each response connected to its request while graph
code schedules work independently of the transport thread. Graph handlers
need both per-request computation and access to all active requests.

## Decision

One server wiring call accepts routes and their handler graphs. It wires the
request queue and response sink internally. A handler for one request runs
through `map()`; a dictionary handler receives requests keyed by request ID and returns
responses with the same keys. Each request remains present until completion,
disconnection, or timeout.

The source owns one transport runtime per graph run. It publishes one internal
runtime handle for the reply sink and a public bound-port time series. The
transport runtime owns response correlation and socket operations. Queue
messages contain public request data.

The runtime handle uses `TS<Opaque<ServerRuntime*>>` so the transport remains
an extension-private C++ type.

## Consequences

Graph authors supply the handler's response through its output. They do not
wire a separate reply destination or copy request IDs into response values.
Port zero and a bound-port output support OS-assigned ports and graph-visible
readiness. The optional extension owns the transport dependencies.

See the [HTTP server extension](../../../extensions/web/README.md) for the public
API and lifecycle.
