# Binary WebSocket connections

WebSocket connects a graph to a persistent stream of binary messages. The current API
is C++ only. Build the [web extension](index.md#web), link `bg::web` and `bg::library`, and use the
realtime engine.

## Serving a connection

`websocket_handler(path, graph)` registers a route on the HTTP listener. The handler
receives `TSB<BinaryWebSocketServerRequest>` and returns
`TSB<BinaryWebSocketResponse>`. Include `<bg/web/http_server.hpp>`.

```cpp
#include <bg/library.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/http_server.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::web;

// Construct the handshake response once before wiring.
auto memory = allocator{};
auto accepted = ScalarValue<Bundle<HttpResponse>>::from_literal(
    {{"status", i64{101}}, {"headers", {}}, {"body", std::span<const std::byte>{}}}, memory);

auto echo = graph([&](wp<TSB<BinaryWebSocketServerRequest>> request) {
    return bundle<BinaryWebSocketResponse>(
        "connect_response"_n = operators::const_(accepted),
        "message"_n = operators::emit(request->messages),
        "close"_n = operators::nothing[type<TS<Bundle<WebSocketClose>>>]());
});
auto route = websocket_handler("/socket", echo);
```

Invoke `route()` in the application graph and register the listener with
`register_http_server(catalogue)`, as in the [HTTP server](web.md#serving-a-request).
The response accepts the handshake and echoes each received binary message.


| Request field | Value |
| --- | --- |
| `connect_request` | `TS<Bundle<HttpServerRequest>>` for the upgrade request |
| `messages` | `TS<List<bytes>>` containing received binary messages in arrival order |

| Response field | Value |
| --- | --- |
| `connect_response` | `TS<Bundle<HttpResponse>>`; status 101 accepts, 400–599 rejects |
| `message` | `TS<bytes>`; each tick sends a complete binary message |
| `close` | `TS<Bundle<WebSocketClose>>`; fields `code` and `reason` |

A successful handshake has an empty body. A selected `Sec-WebSocket-Protocol` must
have been offered by the client. HTTP and WebSocket routes can share a path. Dictionary
handlers and response bundles with extra outputs follow the HTTP handler pattern.

## Connecting as a client

Include `<bg/web/websocket_client.hpp>`. Register `websocket_client_impl()` in the
service catalogue, then connect a request bundle to `websocket_client`:

```cpp
catalogue.register_impl(websocket_client, websocket_client_impl(), {});
auto replies = websocket_client(requests);
```

The input is `TSB<BinaryWebSocketClientRequest>`. `connect_request` carries a
`Bundle<WebSocketConnectRequest>` with `url` and ordered `headers`; `message` and
`close` have the same types as the server response fields. Each connection tick
replaces the preceding connection for that client.

The output is `TSB<BinaryWebSocketClientReply>` with `connect_response`, `messages`,
and `close`. A successful handshake reports 101. A transport failure before the
handshake reports 599; abnormal closure after admission reports close code 1006.
`WebSocketClientOptions` configures connection/handshake timeout and message size.
The current client supports `ws://`.

## Messages, closing, and authentication

The server accepts binary messages up to 8 MiB. Text messages close the connection
with code 1003. A message and close tick in the same open-connection update send the
message before the close frame. Closing during a handshake cancels the attempt.

Application close codes are 1000–1014 excluding 1004, 1005, and 1006, or 3000–4999.
Reasons contain at most 123 UTF-8 bytes. Invalid close values are sent as 1011.
Removing a server route closes its connections with 1001.

Server routes accept the same [policy service](web-auth.md#authorizing-a-route) as
HTTP. The handler receives identity as its second input. Rejection prevents admission;
later rejection or invalidation closes an accepted connection with 1008. Client
credentials use the shared [authentication scope](web-auth.md#supplying-client-credentials).
