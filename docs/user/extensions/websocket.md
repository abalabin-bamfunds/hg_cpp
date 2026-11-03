# Binary WebSocket connections

If your application exchanges a continuing stream of binary messages, use WebSocket
to keep a connection open between the graph and its peer. Build the
[web extension](index.md#web) and run the graph with the realtime engine.

## Serving a connection

To accept WebSocket connections at an HTTP path, register the handling graph with
`websocket_handler(path, graph)`. Include `<hg/web/http_server.hpp>` in C++, or import
the schemas from `hg_web` in Python.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from hgraph import TSB, TS, compute_node, graph, operators
from hg_web import (
    HttpResponse,
    WebSocketClose,
    WebSocketResponse,
    WebSocketServerRequest,
    websocket_handler,
)

@compute_node
def prefix(message: TS[bytes], topic: TS[str]) -> TS[bytes]:
    return topic.value.encode() + b":" + bytes(message.value)

@graph
def echo(request: TSB[WebSocketServerRequest], topic: TS[str]) -> TSB[WebSocketResponse]:
    messages = operators.emit(operators.getattr(request, "messages"))
    return operators.combine[TSB[WebSocketResponse]](
        connect_response=operators.const_(HttpResponse(status=101)),
        message=prefix(messages, topic),
        close=operators.nothing[TS[WebSocketClose]](),
    )

route = websocket_handler("/socket", echo)
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
#include <hg/library.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/web/http_server.hpp>

using namespace hg;
using namespace hg::literals;
using namespace hg::web;

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
:::
::::

Invoke the route in the application graph. The Python example passes its ambient
input with `route("topic")`; the C++ example has no ambient inputs. Register the
listener with `register_http_server(catalogue)`, as in the
[HTTP server](web.md#serving-a-request). The response accepts the handshake and echoes
each received binary message.


| Request field | Value |
| --- | --- |
| `connect_request` | `TS<Bundle<HttpServerRequest>>` for the upgrade request |
| `messages` | `TS<List<bytes>>` containing received binary messages in arrival order |

| Response field | Value |
| --- | --- |
| `connect_response` | `TS<Bundle<HttpResponse>>`; status 101 accepts, 400–599 rejects |
| `message` | `TS<bytes>`; each tick sends a complete binary message |
| `close` | `TS<Bundle<WebSocketClose>>`; fields `code` and `reason` |

Python annotates message collections as `tuple[bytes, ...]`. Readers receive each
payload as a read-only memoryview into native message storage; call `bytes(value)` to
copy it.

A successful handshake has an empty body. A selected `Sec-WebSocket-Protocol` must
have been offered by the client. HTTP and WebSocket routes can share a path. Dictionary
handlers and response bundles with extra outputs follow the HTTP handler pattern.

## Connecting as a client

To exchange messages with a remote WebSocket server, register a client implementation
in the service catalogue and connect a request bundle to `websocket_client`.
Include `<hg/web/websocket_client.hpp>` in C++, or import `websocket_client` from
`hg_web` in Python:

Python uses `TSB[WebSocketClientRequest]` and `TSB[WebSocketClientReply]`; these
correspond to C++ `TSB<BinaryWebSocketClientRequest>` and
`TSB<BinaryWebSocketClientReply>`.

```cpp
catalogue.register_impl(websocket_client, websocket_client_impl(), {});
auto replies = websocket_client(requests);
```

```python
from hg_web import websocket_client, websocket_client_impl

catalogue.register_impl(websocket_client, websocket_client_impl())
replies = websocket_client(requests)
```

The input is `TSB<BinaryWebSocketClientRequest>`. `connect_request` carries a
`Bundle<WebSocketConnectRequest>` with `url` and ordered `headers`; `message` and
`close` have the same types as the server response fields. Each connection tick
replaces the preceding connection for that client.

The output is `TSB<BinaryWebSocketClientReply>` with `connect_response`, `messages`,
and `close`. A successful handshake reports 101. A transport failure before the
handshake reports 599; abnormal closure after admission reports close code 1006.

`WebSocketClientOptions` in C++ and `websocket_client_impl(...)` in Python configure
the handshake timeout (30 seconds by default), maximum message size (8 MiB), and
optional `ca_file`. Python accepts `handshake_timeout=timedelta(...)`,
`max_message_size=...`, and `ca_file=...`. `register_client_transports(...)` accepts
the same three keyword options when it installs both HTTP and WebSocket clients.
The client supports `ws://` and `wss://`. For WSS, certificate and hostname verification
are enabled. `ca_file` adds a PEM CA file to OpenSSL's configured default trust paths.

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
