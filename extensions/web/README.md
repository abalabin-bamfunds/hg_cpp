# HTTP server extension

`bg_web` connects HTTP requests to graph handlers. The server queues requests into
the graph and sends each handler's response to the corresponding connection.

Enable the extension with `BG_BUILD_WEB=ON`. Enable `BG_BUILD_PYTHON=ON` to build
the Python package. Boost.Asio and Boost.Beast are private transport dependencies.

```python
import bg
from bg_web import HttpServerRequest, HttpResponse, http_server

@bg.compute_node
def echo_response(request: bg.TS[HttpServerRequest]) -> bg.TS[HttpResponse]:
    return HttpResponse(status=200, body=request.value.body)

@bg.graph
def echo(request: bg.TS[HttpServerRequest]) -> bg.TS[HttpResponse]:
    return echo_response(request)

@bg.graph
def application() -> None:
    http_server(routes={"/echo": echo}, host="127.0.0.1", port=8080)

bg.run_graph(application, engine=bg.EvaluationEngine.realtime)
```

`http_server` returns `TS[int]` containing the bound port once the listener is
ready. Use `port=0` to let the operating system assign a port. Each call creates
one server with separate runtime state for each graph run.

Routes match the request path exactly. An unmatched path returns HTTP 404.
The handler reads the HTTP method from the request. A handler has one of these
signatures:

```python
TS[HttpServerRequest] -> TS[HttpResponse]
TSD[int, TS[HttpServerRequest]] -> TSD[int, TS[HttpResponse]]
```

The first form uses `map()` to create one graph instance per request. The second
form receives all active requests for its route. Its response keys must match
the request keys. The first response completes a request. Completion,
disconnection, or timeout removes the request. Responses for removed requests
are ignored.

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
Request bodies are limited to 8 MiB. Request reads and handler responses each
time out after 30 seconds.

The C++ entry point is declared in `<bg/web/http_server.hpp>`:

```cpp
bg::web::http_server(
    {{"/echo", echo}},
    {.host = "127.0.0.1", .port = 8080});
```

C++ handlers use `TS<Bundle<HttpServerRequest>>` and
`TS<Bundle<HttpResponse>>`, or dictionaries of those time series keyed by
`i64`. Link the optional `bg::web` target.
