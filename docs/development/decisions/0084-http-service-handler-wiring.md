# HTTP service handler wiring

Status: accepted

## Context

HTTP handlers need to be wired wherever their graph inputs and outputs are
available. A server must route each request only to the handler registered for
its path, and a handler must retain its request identity through response
completion.

## Decision

`http_handler(path, graph)` is a callable wiring template backed by a request/reply
service client. Its first graph input receives one request or the route's request
dictionary. Remaining inputs are supplied at the call site. The handler returns
its response time series; other graph outputs remain available to the caller.

The server implementation owns a path-to-service-client map. It dispatches each
request to one client ID. Removing a client withdraws its route, removes its
pending requests, and removes its outer request dictionary entry. A response is
accepted only while its request and client ID remain active.

`http_server()` returns the bound port after the initial route set is installed.
The implementation owns one transport runtime per graph run and closes its
connections when the graph stops.

## Consequences

Graph authors can wire handlers in separate subgraphs and pass ambient inputs or
use additional outputs without a central route table. The service boundary
correlates transport requests and graph responses. Route conflicts fail during
graph evaluation.

See the [HTTP extension](../../../extensions/web/README.md) for the public API and
lifecycle.
