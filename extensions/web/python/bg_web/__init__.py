"""HTTP client and server primitives for bg graphs."""

from datetime import timedelta

import bg

from ._bg_web import _http_client_impl, _register_schemas, http_server


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
    "http_server",
    "http_client",
    "http_client_impl",
]
