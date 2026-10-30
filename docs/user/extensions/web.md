# HTTP clients and servers

The web extension connects HTTP requests and responses to graph calculations.
HTTP has Python and C++ APIs. Build the [web extension](index.md#web), import from
`bg_web` in Python, or include `<bg/web/http_server.hpp>` and
`<bg/web/http_client.hpp>` in C++.

## Serving a request

`http_handler(path, graph)` connects one route to a graph. The graph receives a request
and returns a response. Each active request gets a separate graph instance, so it can
wait for other graph inputs before responding.

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import EvaluationEngine, ServiceCatalogue, TS, compute_node, graph, run_graph
from bg_web import HttpResponse, HttpServerRequest, http_handler, http_server, register_http_server

@compute_node
def echo(request: TS[HttpServerRequest]) -> TS[HttpResponse]:
    return HttpResponse(body=request.value.body)

@graph
def echo_graph(request: TS[HttpServerRequest]) -> TS[HttpResponse]:
    return echo(request)

route = http_handler("/echo", echo_graph)

@graph
def application() -> None:
    route()
    http_server()

catalogue = ServiceCatalogue()
register_http_server(catalogue, host="127.0.0.1", port=8080)
run_graph(application, catalogue=catalogue, engine=EvaluationEngine.realtime)
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/web/http_server.hpp>

using namespace bg;
using namespace bg::web;

int main() {
    const auto echo = node([](In<TS<Bundle<HttpServerRequest>>> request,
                              Out<TS<Bundle<HttpResponse>>> response) {
        auto memory = allocator{};
        const auto body = scalar_cast<bytes>(request.value()[5]);
        const auto reply = ScalarValue<Bundle<HttpResponse>>::from_literal(
            {{"status", i64{200}}, {"headers", {}}, {"body", body}}, memory);
        response.set_value(reply.view());
    });
    const auto route = http_handler("/echo", echo);

    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue, {.host = "127.0.0.1", .port = 8080});
    run_graph(
        [&] {
            (void)route();
            (void)http_server();
        },
        GraphRunOptions{.services = &catalogue, .engine = EvaluationEngine::realtime});
}
```
:::

:::{tab-item} Build and run

The [C++ example project](index.md#web) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::


For an application running on port 8080, this request returns its body:

```sh
curl --data 'hello' http://127.0.0.1:8080/echo
```

`register_http_server` installs one listener in the service catalogue. `http_server()`
publishes its bound port when the listener and initial routes are ready. `port=0`
selects an available port; an explicit port supports a known application address.
Use the realtime engine so the graph can wait for network events.

Keep each C++ route callable alive through graph compilation and execution. It owns the
handler definition. Bind and listen errors fail startup. Stopping the graph closes the
listener and its connections.

## Request and response values

| Value | Fields |
| --- | --- |
| `HttpServerRequest` | `method`, `target`, `path`, `query`, `headers`, `body`, `peer` |
| `HttpPeer` | `address`, `port` |
| `HttpResponse` | `status`, `headers`, `body` |
| `HttpClientRequest` | `method`, `url`, `headers`, `body` |

`target` retains the raw request target. `path` and query names and values are
percent-decoded; query decoding also converts `+` to a space. Headers and query fields
are ordered pairs and preserve duplicate names. Bodies are byte sequences. Python
reads them as readonly memoryviews; use `bytes(body)` when another API requires bytes.

In Python, `HttpResponse()` defaults to status 200 with empty headers and body.
`HttpClientRequest(url)` defaults to GET with empty headers and body. In C++, these
schemas are compound scalars, for example `TS<Bundle<HttpResponse>>`.

## Route lifetime and extra outputs

Routes match decoded paths exactly. An unmatched path returns 404. The first response
completes a request. Completion, disconnection, route removal, and timeout remove its
graph instance; later responses for it are ignored.

A handler's first argument receives the request. Remaining arguments come from the
route call, so all request instances can use shared changing values:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
orders_route = http_handler("/orders", orders)

@graph
def application(prices: TSD[str, TS[float]]) -> None:
    orders_route(prices)
    http_server()
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
auto orders_route = http_handler("/orders", orders);
auto application = [&](wp<TSD<string, TS<double>>> prices) {
    orders_route(prices);
    http_server();
};
```
:::
::::


A handler can return a bundle with a `response` field and additional outputs. A
single-request route returns results keyed by request ID; use `map` to select additional
per-request outputs. A dictionary handler instead accepts
`TSD[int, TS[HttpServerRequest]]` in Python or
`TSD<i64, TS<Bundle<HttpServerRequest>>>` in C++. Its response dictionary uses the
same request IDs.

## Making a request

`http_client(request)` returns responses for that service client. Register
`http_client_impl()` in the catalogue before running the graph.

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta
from bg import EvaluationEngine, ServiceCatalogue, TS, compute_node, graph, run_graph
from bg.operators import const_
from bg_web import HttpClientRequest, HttpResponse, http_client, http_client_impl

@compute_node
def show(response: TS[HttpResponse]) -> None:
    print(f"{response.value.status}: {bytes(response.value.body).decode()}")

@graph
def application() -> None:
    request = const_(HttpClientRequest(
        "http://127.0.0.1:8080/echo", method="POST", body=b"hello"))
    show(http_client(request))

catalogue = ServiceCatalogue()
catalogue.register_impl(http_client, http_client_impl())
run_graph(application, catalogue=catalogue, engine=EvaluationEngine.realtime,
          run_time=timedelta(seconds=5))
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/web/http_client.hpp>

#include <chrono>
#include <iostream>
#include <span>
#include <string_view>

using namespace bg;
using namespace bg::web;

int main() {
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(http_client, http_client_impl(), {});

    const auto make_request = node([](Out<TS<Bundle<HttpClientRequest>>> output) {
        const auto body = std::as_bytes(std::span{std::string_view{"hello"}});
        auto memory = allocator{};
        const auto request = ScalarValue<Bundle<HttpClientRequest>>::from_literal(
            {{"method", "POST"}, {"url", "http://127.0.0.1:8080/echo"}, {"headers", {}}, {"body", body}}, memory);
        output.set_value(request.view());
    });
    const auto print_response = node([](In<TS<Bundle<HttpResponse>>> response) {
        const auto value = response.value();
        const auto body = scalar_cast<bytes>(value[2]).value();
        std::cout << scalar_cast<i64>(value[0]).value() << ": "
                  << std::string{reinterpret_cast<const char *>(body.data()), body.size()} << '\n';
    });

    run_graph(
        [&] { print_response(http_client(make_request())); },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{5}});
}
```
:::

:::{tab-item} Build and run

The [C++ example project](index.md#web) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::


Each valid input tick starts a request. Responses arrive in completion order, which can
differ from submission order. Use separate service calls when requests need separate
response time series. Removing a client cancels its outstanding requests.

HTTP error statuses such as 401 and 500 are ordinary responses. Transport and protocol
failures produce status 599 with an error message in the body. Redirect following and
retries are disabled. HTTPS verifies the certificate and hostname.

## Testing response handling

Response processing can be tested with literal inputs, without a listener or a client
transport. These inputs include successful and error statuses:

<!-- example-libraries: bg::web -->
<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, compute_node, graph, run_graph
from bg_web import HttpResponse

@compute_node
def response_status(response: TS[HttpResponse]) -> TS[int]:
    return response.value.status

@graph
def statuses(response: TS[HttpResponse]) -> int:
    return response_status(response)

print(run_graph(statuses, [
    HttpResponse(status=200),
    HttpResponse(status=401),
    HttpResponse(status=500),
]))
```

Output:

```text
[200, 401, 500]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/web/types.hpp>

#include <iostream>
#include <span>

using namespace bg;
using namespace bg::web;

int main() {
    const auto response_status = node([](In<TS<Bundle<HttpResponse>>> response) {
        return scalar_cast<i64>(response.value()[0]).value();
    });
    const auto statuses = [&](wp<TS<Bundle<HttpResponse>>> response) {
        return response_status(response);
    };
    std::cout << run_graph(statuses, {
        {{"status", i64{200}}, {"headers", {}}, {"body", std::span<const std::byte>{}}},
        {{"status", i64{401}}, {"headers", {}}, {"body", std::span<const std::byte>{}}},
        {{"status", i64{500}}, {"headers", {}}, {"body", std::span<const std::byte>{}}},
    }) << '\n';
}
```

Output:

```text
[200, 401, 500]
```
:::

:::{tab-item} Build and run

The [C++ example project](index.md#web) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

## Transport options and limits

| Setting | Default or accepted values |
| --- | --- |
| Server address | `127.0.0.1`, port 0 |
| Server request body limit | 8 MiB |
| Server request read, handler, and response write timeout | 30 seconds each |
| Client timeout | 30 seconds, at least one millisecond |
| Client response body limit | 8 MiB |
| Client URL | `http` or `https`, without embedded credentials or fragments |
| Application response status | 200–599 |

Configure Python clients with
`http_client_impl(timeout=timedelta(seconds=10), max_response_body_size=1024 * 1024)`.
C++ uses `http_client_impl(HttpClientOptions{...})`. Client timeouts bound the transfer;
bounding DNS resolution also requires libcurl with asynchronous DNS support.

The transport controls routing and framing headers. Client requests cannot set `Host`,
`Connection`, `Content-Length`, `Transfer-Encoding`, `Upgrade`, `Trailer`, `TE`,
`Keep-Alive`, `Proxy-Connection`, or `Expect`. Header values cannot contain line breaks.
Server responses cannot set `Connection` or `Transfer-Encoding`; a supplied
`Content-Length` must match the body length. HEAD, 204, 205, and 304 responses omit
the body. HEAD and 304 use the supplied body to determine representation length.

The server supports HTTP/1.1 keep-alive with one active request per connection.
For protected routes and authenticated clients, see [authentication](web-auth.md).

## Complete HTTP round trip

This program starts an ephemeral listener, posts a body to it, and prints the response.
The bound port becomes the client request's URL. Both transports use the same catalogue;
the run ends after two seconds.

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from datetime import timedelta

from bg import EvaluationEngine, ServiceCatalogue, TS, compute_node, graph, run_graph
from bg_web import HttpClientRequest, HttpResponse, HttpServerRequest
from bg_web import http_client, http_client_impl, http_handler, http_server, register_http_server


def main():
    @compute_node
    def echo(request: TS[HttpServerRequest]) -> TS[HttpResponse]:
        return HttpResponse(status=200, body=request.value.body)

    @graph
    def echo_graph(request: TS[HttpServerRequest]) -> TS[HttpResponse]:
        return echo(request)

    route = http_handler("/echo", echo_graph)

    @compute_node
    def make_request(port: TS[int]) -> TS[HttpClientRequest]:
        return HttpClientRequest(f"http://127.0.0.1:{port.value}/echo", method="POST", body=b"hello")

    @compute_node
    def print_response(response: TS[HttpResponse]) -> None:
        print(f"{response.value.status}: {bytes(response.value.body).decode()}")

    @graph
    def application() -> None:
        route()
        print_response(http_client(make_request(http_server())))

    catalogue = ServiceCatalogue()
    register_http_server(catalogue)
    catalogue.register_impl(http_client, http_client_impl())
    run_graph(
        application,
        catalogue=catalogue,
        engine=EvaluationEngine.realtime,
        run_time=timedelta(seconds=2),
    )


if __name__ == "__main__":
    main()
```

Response:

```text
200: hello
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/web/http_client.hpp>
#include <bg/web/http_server.hpp>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>
#include <string>

using namespace bg;
using namespace bg::web;

int main() {
    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue);
    catalogue.register_impl(http_client, http_client_impl(), {});

    const auto echo = node([](In<TS<Bundle<HttpServerRequest>>> request,
                              Out<TS<Bundle<HttpResponse>>> response) {
        auto memory = allocator{};
        const auto body = scalar_cast<bytes>(request.value()[5]);
        const auto value = ScalarValue<Bundle<HttpResponse>>::from_literal(
            {{"status", i64{200}}, {"headers", {}}, {"body", body}}, memory);
        response.set_value(value.view());
    });
    const auto route = http_handler("/echo", echo);

    const auto make_request = node([](In<TS<i64>> port, Out<TS<Bundle<HttpClientRequest>>> request) {
        const auto body = std::as_bytes(std::span{std::string_view{"hello"}});
        const auto url = "http://127.0.0.1:" + std::to_string(*port) + "/echo";
        auto memory = allocator{};
        const auto value = ScalarValue<Bundle<HttpClientRequest>>::from_literal(
            {{"method", "POST"}, {"url", url}, {"headers", {}}, {"body", body}}, memory);
        request.set_value(value.view());
    });
    const auto print_response = node([](In<TS<Bundle<HttpResponse>>> response) {
        const auto value = response.value();
        const auto status = scalar_cast<i64>(value[0]).value();
        const auto body = scalar_cast<bytes>(value[2]).value();
        std::cout << status << ": " << std::string{reinterpret_cast<const char *>(body.data()), body.size()} << '\n';
    });

    run_graph(
        [&] {
            (void)route();
            const auto port = http_server();
            print_response(http_client(make_request(port)));
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{2}});
}
```

Response:

```text
200: hello
```
:::

:::{tab-item} Build and run

The [C++ example project](index.md#web) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

For a proxy environment, include `127.0.0.1` in `NO_PROXY` for the loopback examples.

The current HTTP client can log `HTTP response queue is closed` during graph shutdown
after a successful response.
