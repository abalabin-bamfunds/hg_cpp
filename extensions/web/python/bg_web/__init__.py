"""HTTP client and server primitives for bg graphs."""

from datetime import timedelta
import inspect
from functools import update_wrapper

import bg

from ._bg_web import (
    _http_client_impl,
    _http_handler_input_is_dictionary,
    _http_response_field,
    _http_server_impl,
    _register_schemas,
)


class HttpPeer:
    address: str
    port: int


class HttpServerRequest:
    method: str
    target: str
    path: str
    query: list[tuple[str, str]]
    headers: list[tuple[str, str]]
    body: bytes
    peer: HttpPeer


class HttpClientRequest:
    method: str
    url: str
    headers: list[tuple[str, str]]
    body: bytes

    def __init__(self, url, method="GET", headers=(), body=b""):
        self.method = method
        self.url = url
        self.headers = list(headers)
        self.body = body


class HttpResponse:
    status: int
    headers: list[tuple[str, str]]
    body: bytes

    def __init__(self, status=200, headers=(), body=b""):
        self.status = status
        self.headers = headers
        self.body = body


_register_schemas(HttpPeer, HttpServerRequest, HttpClientRequest, HttpResponse)


@bg.service
def _http_handler_service(
    route: bg.TS[str, bg.final],
    responses: bg.TSD[int, bg.TS[HttpResponse]],
) -> bg.TSD[int, bg.TS[HttpServerRequest]]:
    """Request/reply boundary used by one HTTP handler client."""
    raise RuntimeError("the HTTP handler service is a declaration")


@bg.reference_service
def _http_server_port_service() -> bg.TS[int]:
    """Return the port bound by the registered HTTP server implementation."""
    raise RuntimeError("the HTTP server port is a service declaration")


class HttpHandler:
    def __init__(self, path, graph):
        if not isinstance(path, str) or not path.startswith("/") or any(c in path for c in "?#"):
            raise ValueError("HTTP handler paths must be absolute paths without query or fragment")
        if not isinstance(graph, bg.GraphDefinition):
            raise TypeError("http_handler expects a decorated @graph definition")

        parameters = tuple(inspect.signature(graph.function).parameters.values())
        if not parameters:
            raise TypeError("an HTTP handler graph must take a request input")
        self.path = path
        self.graph = graph
        self.dictionary_input = _http_handler_input_is_dictionary(graph._native_definition)
        self.parameters = parameters[1:]
        self.signature = inspect.Signature(self.parameters)
        update_wrapper(self, graph.function)
        self.__signature__ = self.signature

    def __call__(self, *args, **kwargs):
        bound = self.signature.bind(*args, **kwargs)
        bound.apply_defaults()

        client_id = bg.service_client_id()
        requests = _http_handler_service.client_replies(client_id)
        if self.dictionary_input:
            result = self.graph(requests, *bound.args, **bound.kwargs)
        else:
            mapped_positional = tuple(bg.pass_through(value) for value in bound.args)
            mapped_named = {name: bg.pass_through(value) for name, value in bound.kwargs.items()}
            result = bg.map(self.graph, requests, *mapped_positional, **mapped_named)

        responses = _http_response_field(result)
        route = bg.operators.const_(self.path)
        _http_handler_service.client_requests(client_id, route, responses)
        return result


def http_handler(path, graph):
    """Create a route that runs a graph for HTTP requests and returns its outputs."""
    return HttpHandler(path, graph)


def register_http_server(catalogue, host="127.0.0.1", port=0):
    """Register an HTTP server implementation in a service catalogue."""
    @bg.graph
    def implementation():
        service_requests = _http_handler_service.implementation_requests()
        routes = bg.operators.getattr(service_requests, bg.Symbol("route"))
        responses = bg.operators.getattr(service_requests, bg.Symbol("responses"))
        outputs = _http_server_impl(routes, responses, host, port)
        _http_handler_service.implementation_replies(bg.operators.getattr(outputs, bg.Symbol("requests")))
        _http_server_port_service.implementation_replies(bg.operators.getattr(outputs, bg.Symbol("port")))

    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(_http_handler_service)
    registration.register_impl(_http_server_port_service)


def http_server():
    """Return the bound port after the server installs its handler routes."""
    return _http_server_port_service()


@bg.service
def http_client(request: bg.TS[HttpClientRequest]) -> bg.TS[HttpResponse]:
    """Send each request update over HTTP and return its response."""
    raise RuntimeError("http_client is a service declaration")


def http_client_impl(timeout=timedelta(seconds=30), max_response_body_size=8 * 1024 * 1024):
    """Build a graph implementation for the HTTP client service."""
    if not isinstance(timeout, timedelta):
        raise TypeError("timeout must be a datetime.timedelta")
    timeout_ms = timeout.days * 86_400_000 + timeout.seconds * 1000 + timeout.microseconds // 1000
    if timeout_ms <= 0:
        raise ValueError("timeout must be at least one millisecond")
    if (
        isinstance(max_response_body_size, bool)
        or not isinstance(max_response_body_size, int)
        or max_response_body_size < 0
    ):
        raise ValueError("max_response_body_size must be a non-negative integer")

    @bg.graph
    def implementation(request: bg.TSD[int, bg.TS[HttpClientRequest]]) -> bg.TSD[int, bg.TS[HttpResponse]]:
        return _http_client_impl(request, timeout_ms, max_response_body_size)

    return implementation

__all__ = [
    "HttpPeer",
    "HttpServerRequest",
    "HttpClientRequest",
    "HttpResponse",
    "http_handler",
    "register_http_server",
    "http_server",
    "http_client",
    "http_client_impl",
]
