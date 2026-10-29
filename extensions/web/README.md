# HTTP extension

`bg_web` connects HTTP clients and servers to graphs. Transport threads submit
incoming requests and completed client responses through engine queues.

Enable the extension with `BG_BUILD_WEB=ON`. Enable `BG_BUILD_PYTHON=ON` to build
the Python package. Boost.Asio and Boost.Beast provide the server transport;
Boost.URL parses request targets. libcurl provides the client transport.
These dependencies are private to the extension.

## Server

```python
import bg
from bg_web import HttpServerRequest, HttpResponse, http_handler, http_server, register_http_server

@bg.compute_node
def echo_response(request: bg.TS[HttpServerRequest]) -> bg.TS[HttpResponse]:
    return HttpResponse(status=200, body=request.value.body)

@bg.graph
def echo(request: bg.TS[HttpServerRequest]) -> bg.TS[HttpResponse]:
    return echo_response(request)

@bg.graph
def application() -> None:
    http_handler("/echo", echo)()
    http_server()

catalogue = bg.ServiceCatalogue()
register_http_server(catalogue, host="127.0.0.1", port=8080)
bg.run_graph(application, catalogue=catalogue, engine=bg.EvaluationEngine.realtime)
```

`register_http_server` installs the server implementation in a service
catalogue. `http_server()` returns `TS[int]` containing the bound port after the
listener is ready. Use `port=0` to let the operating system assign a port. Each
implementation owns one server with separate runtime state for each graph run.
Runtime failures are logged and terminate the affected request or connection;
the listener continues serving other entries. A duplicate route registration
is logged and rejected, leaving the existing route active. Bind and listen
failures prevent startup and propagate to the graph.

`http_handler(path, graph)` creates a callable handler. Invoke it wherever the
route should be wired. The graph's first input receives the request. Remaining
inputs are ambient graph arguments supplied to the handler call. A single-request
handler returns a result keyed by request ID; use `map()` to project additional
outputs from those per-request results. The handler output is the response, or a
bundle containing a `response` field and any additional outputs. A dictionary
handler can accept all active requests and return response values keyed by the
same request IDs.
Routes match the request path exactly. Each route receives only its own
requests. An unmatched path returns HTTP 404.

The request graph can use either signature:

```python
TS[HttpServerRequest] -> TS[HttpResponse]
TSD[int, TS[HttpServerRequest]] -> TSD[int, TS[HttpResponse]]
```

The first form uses `map()` to create one graph instance per request. The second
form receives all active requests for its route. Its response keys must match
the request keys. The first response completes a request. Completion,
disconnection, route removal, or timeout removes the request. Responses for
removed requests are ignored.

`HttpServerRequest` contains `method`, `target`, `path`, `query`, `headers`,
`body`, and `peer`. `peer` contains the remote `address` and `port`.
`target` retains the raw request target. `path` is percent-decoded; query names
and values are percent-decoded and convert `+` to a space. Headers and query
parameters preserve duplicate names and their order. `HttpResponse` contains
`status`, `headers`, and `body`. Python's `HttpResponse()` defaults to status 200
with empty headers and body. Bodies use native bytes; Python receives
read-only memoryviews that retain their storage. Use `bytes(view)` when an API
requires Python's built-in `bytes` type.

The transport controls response framing. Application responses use status codes
200–599 and cannot set `Connection` or `Transfer-Encoding`. A supplied
`Content-Length` must match the body length. HEAD, 204, 205, and 304 responses
omit the body. For HEAD and 304, the body supplies the representation length.

Network operations run on the transport thread. Graph stop closes the listener
and connections and joins that thread. The initial transport supports
HTTP/1.1 and keep-alive, with one active request per connection.
HTTP/1.1 requests require one valid `Host` header.
Request bodies are limited to 8 MiB. Request reads, handler responses, and
response writes each time out after 30 seconds.

## WebSocket server

The WebSocket server API is currently C++ only.

`websocket_handler(path, graph)` registers a WebSocket route on the HTTP server's
listener. HTTP and WebSocket routes can use the same path. A WebSocket handler
receives `TSB<BinaryWebSocketServerRequest>` for one connection or a
dictionary keyed by connection ID. The request contains the HTTP
`connect_request` and a time series of binary `messages`.

The handler returns `TSB<BinaryWebSocketResponse>` or a bundle with a
`response` field. `connect_response` accepts a connection with status 101 and an
empty body, or rejects it with an HTTP status from 400 through 599. Its headers
are sent with the handshake. A `Sec-WebSocket-Protocol` response header must
select one protocol offered by the client. `message` sends a binary payload;
`close` sends a close code and UTF-8 reason. Close codes must be 1000–1014
excluding 1004, 1005, and 1006, or 3000–4999. Reasons are limited to 123 UTF-8
bytes. Invalid close values are sent as close code 1011. Incoming text messages
close the connection with code 1003. Incoming message payloads are limited to
8 MiB.

An authenticated route uses the same policy service as `http_handler`:

```cpp
auto route = bg::web::websocket_handler("/socket", handler, policy);
```

The handler receives the identity as its second input. A rejected connection
receives the policy's HTTP response before the WebSocket handshake. If the
policy later rejects or invalidates the identity, the server closes the
connection with code 1008. Removing a route closes its active connections with
code 1001.

The C++ entry points are declared in `<bg/web/http_server.hpp>`:

```cpp
auto catalogue = bg::ServiceCatalogue{};
bg::web::register_http_server(catalogue, {.host = "127.0.0.1", .port = 8080});
auto echo_route = bg::web::http_handler("/echo", echo);

bg::run_graph([&] {
    (void)echo_route();
    (void)bg::web::http_server();
}, bg::GraphRunOptions{.services = &catalogue,
                       .engine = bg::EvaluationEngine::realtime});
```

C++ handlers use `TS<Bundle<HttpServerRequest>>` for one request or
`TSD<i64, TS<Bundle<HttpServerRequest>>>` for all route requests. A single-request
handler returns `TS<Bundle<HttpResponse>>` or a bundle with a `response` field
and additional outputs. Its mapped result is `TSD<i64, REF<HANDLER_OUTPUT>>`.
A dictionary handler returns `TSD<i64, TS<Bundle<HttpResponse>>>` or a bundle
with a `response` field containing that dictionary and additional outputs. Link
the optional `bg::web` target. Keep each returned C++ route callable alive through
graph compilation and execution. The callable owns its handler definition.

## Server authentication

`http_handler(path, graph, policy=policy_service)` selects an ordinary service
that authorizes each request. Register policy implementations in the service
catalogue alongside the server implementation:

```python
from bg_web import AuthDecision

@bg.schema
class User:
    name: str

@bg.service
def orders_access(request: bg.TS[HttpServerRequest]) -> bg.TS[AuthDecision[User]]:
    ...

@bg.graph
def orders(request: bg.TS[HttpServerRequest], identity: bg.TS[User],
           prices: bg.TSD[str, bg.TS[float]]) -> bg.TS[HttpResponse]:
    return order_response(request, identity, prices)

orders_route = http_handler("/orders", orders, policy=orders_access)

@bg.graph
def application(prices: bg.TSD[str, bg.TS[float]]) -> None:
    orders_route(prices)

catalogue.register_impl(orders_access, orders_access_impl)
```

The application supplies `order_response` and `orders_access_impl`. Different
routes can select different policy services. Their implementations can share
authentication services and can be replaced through the catalogue for tests.
A missing implementation fails during graph wiring.

A policy returns `AuthDecision[User].accept(user)` or
`AuthDecision[User].reject(HttpResponse(...))`. A decision contains exactly one
of `identity` and `rejection`. A rejection can be any valid HTTP response,
including a redirect or a 401 response with `WWW-Authenticate` headers.

While the decision is pending, no per-request handler graph starts. Acceptance
starts the handler with the request and typed identity as its first two inputs.
The handler call exposes only its remaining ambient arguments and preserves its
additional outputs. Rejection sends the policy response without starting that
request's handler graph.

A protected dictionary handler takes request and identity dictionaries as its
first two inputs. Both contain only accepted request IDs. The dictionary handler
retains its lifecycle for the complete route. Rejection responses go directly to
the HTTP service; they do not produce handler-specific additional outputs.

The C++ decision type is `bg::web::AuthDecision<IDENTITY>` from
`<bg/web/auth.hpp>`. Use `bg::web::auth::accept<IDENTITY>(identity)` and
`bg::web::auth::reject<IDENTITY>(response)` to construct decisions; both
factories also accept an allocator as their first argument.
Pass the policy service as the third argument to C++ `http_handler` and keep the
service declaration alive through handler wiring. The handler uses that original
declaration's catalogue identity.

## HTTP client

`http_client` is a request/reply service. Register its transport implementation
in the graph's service catalogue:

```python
import bg
from bg_web import HttpClientRequest, HttpResponse, http_client, http_client_impl

@bg.compute_node
def show_response(response: bg.TS[HttpResponse]) -> None:
    print(response.value.status, bytes(response.value.body))

@bg.graph
def application() -> None:
    request = bg.operators.const_(HttpClientRequest("http://127.0.0.1:8080/echo"))
    show_response(http_client(request))

catalogue = bg.ServiceCatalogue()
catalogue.register_impl(http_client, http_client_impl())
bg.run_graph(application, catalogue=catalogue,
             engine=bg.EvaluationEngine.realtime)
```

`HttpClientRequest` contains `method`, `url`, `headers`, and `body`. Its Python
constructor requires `url` and defaults to method `GET`, empty headers, and an
empty body. Headers preserve duplicate names and order. Bodies use the same
native bytes representation as server requests and responses.

The transport controls request routing and framing. Requests cannot supply
`Host`, `Connection`, `Content-Length`, `Transfer-Encoding`, `Upgrade`,
`Trailer`, `TE`, `Keep-Alive`, `Proxy-Connection`, or `Expect` headers. Header
values must be Latin-1 text without line breaks. URLs must use `http` or `https`
and cannot contain credentials or fragments.

Every valid request tick starts a request. Responses tick in completion order
under the originating service client ID. A client can have several outstanding
requests. Graph code creates separate service clients when it needs a separate
response time series for each request. Removing a client cancels its outstanding
requests. Completions queued before cancellation cleanup remain ordered before
the reply removal. Graph stop cancels transport work and joins the transport
thread.

HTTP error statuses, including 401 and 500, are ordinary responses. A local
transport failure produces `HttpResponse(status=599)` with an error message in
the body. This includes DNS, connection, timeout, and protocol failures.
599 is a nonstandard status; an actual server response with that status has the
same representation.
Runtime failures are logged. A libcurl multi-engine failure returns 599 for
each active request, replaces the engine, and does not replay requests.

`http_client_impl(timeout=timedelta(seconds=30),
max_response_body_size=8 * 1024 * 1024)` configures each implementation. The
timeout has millisecond resolution and must be at least one millisecond. It
bounds the transfer. Bounding DNS resolution also requires a libcurl
build with asynchronous DNS support. The response body limit defaults to
8 MiB. The client uses libcurl multi for concurrent
HTTP/1.1 requests, connection reuse, and DNS caching. HTTPS verifies the server
certificate and hostname. Redirect following is disabled. The adaptor adds no
retry policy.

The C++ entry points are declared in `<bg/web/http_client.hpp>`:

```cpp
auto catalogue = bg::ServiceCatalogue{};
catalogue.register_impl(bg::web::http_client, bg::web::http_client_impl(), {});
```

The client signature is `TS<Bundle<HttpClientRequest>> ->
TS<Bundle<HttpResponse>>`. Pass `HttpClientOptions` to `http_client_impl()` to
configure the timeout and response body limit. One implementation owns one
transport thread shared by its service clients, with separate runtime state
for each graph run.

## Client authentication values

Client authentication values use the `Auth` schema hierarchy. `AnonymousAuth`
is empty. `BasicAuth`, `DigestAuth`, and `NtlmAuth` contain `username` and
`password`; `NegotiateAuth` is empty and uses the platform's default
credentials. `BearerAuth` contains `token`; `HeaderAuth` contains ordered
`headers`. Python exports constructors with these field names, and a `TS[Auth]`
input preserves the concrete derived class.

The native transport boundary accepts these values. HTTP handles all seven
derived schemas. WebSocket handles `AnonymousAuth`, `BasicAuth`, `BearerAuth`,
and `HeaderAuth`; other schemas return status 599 before the handshake. The
public client service implementations do not yet expose authentication input;
public client authentication registration is a later slice.

## WebSocket client

`websocket_client` connects to a binary `ws://` endpoint. Its request is
`TSB<BinaryWebSocketClientRequest>` with a `connect_request` containing a URL
and ordered handshake headers, a `message` containing bytes, and a `close`
request. Each connect tick starts a new connection for that service client.
The graph submits request updates to the transport thread during the evaluation
that receives them.
Message ticks send complete binary messages. For an open connection, a message
and close request in one update sends the message before the close frame. A
close request during the handshake cancels the attempt and discards any message
from that update.

The reply is `TSB<BinaryWebSocketClientReply>`. `connect_response` contains the
handshake status, headers, and body. A successful handshake reports status 101
and includes the selected subprotocol header when the server returns one.
`messages` contains received binary messages in arrival order. `close` reports
the observed close code and reason. A transport failure before the handshake
returns status 599; an abnormal closure after admission reports code 1006.

The C++ entry points are declared in `<bg/web/websocket_client.hpp>`:

```cpp
auto catalogue = bg::ServiceCatalogue{};
catalogue.register_impl(bg::web::websocket_client, bg::web::websocket_client_impl(), {});
```

`WebSocketClientOptions` configures the timeout for resolve, connect, and
handshake, plus the maximum received message size. The initial client supports
only `ws://`, binary messages, and one active connection per service client.
