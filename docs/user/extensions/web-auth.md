# HTTP authentication

Server policies control whether a request can start its handler graph. Client
credentials supply authentication for requests to a registered origin. Both are graph
calculations, so they can depend on changing graph inputs and services.

## Authorizing a route

A policy is a service that takes an `HttpServerRequest` and returns an
`AuthDecision[User]`. Acceptance supplies a typed identity; rejection supplies an HTTP
response. Register the service implementation in the same catalogue as the listener.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg import TS, service
from bg_web import AuthDecision, HttpServerRequest, http_handler

@service
def access(request: TS[HttpServerRequest]) -> TS[AuthDecision[int]]: ...

orders_route = http_handler("/orders", orders, policy=access)
catalogue.register_impl(access, access_impl)
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
#include <bg/service.hpp>
#include <bg/web/auth.hpp>
#include <bg/web/http_server.hpp>

using namespace bg;
using namespace bg::web;

auto access = service<auto(TS<Bundle<HttpServerRequest>>) -> TS<AuthDecision<i64>>>{
    "access", {"request"}};
auto orders_route = http_handler("/orders", orders, access);
catalogue.register_impl(access, access_impl, {});
```
:::
::::

Here `orders` accepts the request and an integer identity; `access_impl` is the
application's policy implementation. A policy constructs its result as follows:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg_web import AuthDecision, HttpResponse

accepted = AuthDecision[int].accept(42)
denied = AuthDecision[int].reject(HttpResponse(status=403))
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
auto identity = ScalarValue<i64>{42};
auto accepted = auth::accept<i64>(identity.view());

auto memory = allocator{};
auto response = ScalarValue<Bundle<HttpResponse>>::from_literal(
    {{"status", i64{403}}, {"headers", {}}, {"body", std::span<const std::byte>{}}}, memory);
auto denied = auth::reject<i64>(response.view());
```
:::
::::


`http_handler(..., policy=policy)` in Python, or `http_handler(path, handler, policy)`
in C++, adds the identity as the handler's second input. Remaining inputs still come
from the route call. While the policy has no decision, the per-request handler waits.
A rejection completes the request without starting that handler.

Python constructs decisions with `AuthDecision[User].accept(user)` or
`.reject(HttpResponse(...))`. C++ includes `<bg/web/auth.hpp>` and uses
`auth::accept<User>(identity)` or `auth::reject<User>(response)`. A rejection can include
`WWW-Authenticate` headers or a redirect. Keep the C++ service declaration alive through
route wiring.

## Supplying client credentials

`register_client_transports(catalogue, scope)` installs transports sharing an
authentication scope. Use it in place of the standalone client implementations.
It installs HTTP in Python, and HTTP plus WebSocket in C++.
`register_client_auth(scope, origin, graph, ...)` associates an origin with a
credential graph. The graph receives the normalized origin and any supplied ambient
arguments. It returns `TS[Auth]` or a supported derived authentication type.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg import TS, graph
from bg.operators import const_
from bg_web import BearerAuth, register_client_auth, register_client_transports

@graph
def credentials(origin: str) -> TS[BearerAuth]:
    return const_(BearerAuth(token="example-token"))

register_client_transports(catalogue, "api")

# Inside the application graph:
register_client_auth("api", "https://api.example.com", credentials)
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
#include <bg/web/client_auth.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::web;

auto memory = allocator{};
auto token = bundle<BearerAuth>(memory, "token"_n = "example-token");
auto credentials = graph([&](wp<TS<string>>) { return operators::const_(token); });

register_client_transports(catalogue, "api");

// Inside the application graph:
register_client_auth("api", "https://api.example.com", credentials);
```
:::
::::


One credential graph instance serves each demanded origin, shared by its clients.
A matched registration with invalid credentials waits for a valid value within the
request timeout. An unmatched origin uses anonymous access.

| Type | Fields | HTTP | C++ WebSocket |
| --- | --- | --- | --- |
| `AnonymousAuth` | None | Yes | Yes |
| `BasicAuth` | `username`, `password` | Yes | Yes |
| `DigestAuth` | `username`, `password` | Yes | — |
| `NtlmAuth` | `username`, `password` | Yes | — |
| `NegotiateAuth` | None; platform credentials | Yes | — |
| `BearerAuth` | `token` | Yes | Yes |
| `HeaderAuth` | Ordered `headers` | Yes | Yes |

C++ declarations are in `<bg/web/client_auth.hpp>`. Negotiate requires libcurl with
GSSAPI or Windows SSPI support; the application supplies and renews platform credentials.

## Origin matching and updates

Origins use `http`, `https`, `ws`, or `wss`. Normalization lowercases the scheme and
host and removes the default port. An absent path and `/` identify the same origin;
other paths, credentials, queries, and fragments are rejected. HTTP and WebSocket
origins remain distinct.

An exact registration takes precedence over a regular expression. Use Python
`ClientAuthRegex(pattern)` or C++ `ClientAuthRegex{pattern}` for full-origin ECMAScript
matching. More than one match at the selected precedence produces a local 599 response.

The request timeout includes credential waiting. A new HTTP request replaces the same
client's pending request while credentials are unavailable. Once valid credentials
arrive, only the latest pending request is submitted. Client removal cancels the wait.
Credential changes after a WebSocket handshake is submitted do not reconnect it.

A credential graph can call `http_client` to obtain a token when that request has
independent authentication. Its request cannot depend on the token it is obtaining.
