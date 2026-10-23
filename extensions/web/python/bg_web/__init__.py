"""HTTP server nodes for bg graphs."""

from ._bg_web import _register_schemas, http_server


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


class HttpResponse:
    status: int
    headers: list[tuple[str, str]]
    body: bytes

    def __init__(self, status=200, headers=(), body=b""):
        self.status = status
        self.headers = headers
        self.body = body


_register_schemas(HttpPeer, HttpServerRequest, HttpResponse)

__all__ = ["HttpPeer", "HttpServerRequest", "HttpResponse", "http_server"]
