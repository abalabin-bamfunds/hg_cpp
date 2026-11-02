"""HTTP client and server primitives for hg graphs."""

from datetime import timedelta
import inspect
from functools import update_wrapper

import hgraph as hg

from ._hg_web import (
    _http_client_impl,
    _http_authenticate_requests,
    _http_handler_input_is_dictionary,
    _http_merge_responses,
    _http_response_field,
    _http_server_impl,
    _websocket_client_impl,
    _websocket_handler,
    _normalize_client_auth_origin,
    _register_client_transports,
    _register_schemas,
    _register_websocket_time_series_schemas,
    _register_services,
    _validate_client_auth_regex,
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


class WebSocketConnectRequest:
    url: str
    headers: list[tuple[str, str]]

    def __init__(self, url, headers=()):
        self.url = url
        self.headers = list(headers)


class WebSocketClose:
    code: int
    reason: str


class Auth:
    """Base schema for client authentication values."""


class AnonymousAuth(Auth):
    """Explicit anonymous client authentication."""


class BasicAuth(Auth):
    username: str
    password: str


class DigestAuth(Auth):
    username: str
    password: str


class NegotiateAuth(Auth):
    """Use the platform's default Negotiate credentials."""


class NtlmAuth(Auth):
    username: str
    password: str


class BearerAuth(Auth):
    token: str


class HeaderAuth(Auth):
    headers: list[tuple[str, str]]


class _ClientAuthMatcher:
    pattern: str
    regex: bool


_register_schemas(
    HttpPeer,
    HttpServerRequest,
    HttpClientRequest,
    HttpResponse,
    Auth,
    AnonymousAuth,
    BasicAuth,
    DigestAuth,
    NegotiateAuth,
    NtlmAuth,
    BearerAuth,
    HeaderAuth,
    _ClientAuthMatcher,
    WebSocketConnectRequest,
    WebSocketClose,
)


class WebSocketServerRequest:
    connect_request: hg.TS[HttpServerRequest]
    messages: hg.TS[tuple[bytes, ...]]


class WebSocketResponse:
    connect_response: hg.TS[HttpResponse]
    message: hg.TS[bytes]
    close: hg.TS[WebSocketClose]


class WebSocketClientRequest:
    connect_request: hg.TS[WebSocketConnectRequest]
    message: hg.TS[bytes]
    close: hg.TS[WebSocketClose]


class WebSocketClientReply:
    connect_response: hg.TS[HttpResponse]
    messages: hg.TS[tuple[bytes, ...]]
    close: hg.TS[WebSocketClose]


_register_websocket_time_series_schemas(
    WebSocketServerRequest,
    WebSocketResponse,
    WebSocketClientRequest,
    WebSocketClientReply,
)

from .auth import AuthDecision, _policy_graphs


@hg.service
def _http_handler_service(
    route: hg.TS[str, hg.final],
    responses: hg.TSD[int, hg.TS[HttpResponse]],
) -> hg.TSD[int, hg.TS[HttpServerRequest]]:
    """Request/reply boundary used by one HTTP handler client."""
    raise RuntimeError("the HTTP handler service is a declaration")


@hg.service
def _websocket_handler_service(
    route: hg.TS[str, hg.final],
    responses: hg.TSD[int, hg.TSB[WebSocketResponse]],
) -> hg.TSD[int, hg.TSB[WebSocketServerRequest]]:
    """Request/reply boundary used by one WebSocket handler client."""
    raise RuntimeError("the WebSocket handler service is a declaration")


@hg.reference_service
def _http_server_port_service() -> hg.TS[int]:
    """Return the port bound by the registered HTTP server implementation."""
    raise RuntimeError("the HTTP server port is a service declaration")


class HttpHandler:
    def __init__(self, path, graph, policy=None):
        if not isinstance(path, str) or not path.startswith("/") or any(c in path for c in "?#"):
            raise ValueError("HTTP handler paths must be absolute paths without query or fragment")
        if not isinstance(graph, hg.GraphDefinition):
            raise TypeError("http_handler expects a decorated @graph definition")

        parameters = tuple(inspect.signature(graph.function).parameters.values())
        if not parameters:
            raise TypeError("an HTTP handler graph must take a request input")
        self.policy = policy
        self.policy_graphs = None if policy is None else _policy_graphs(policy)
        argument_start = 1 if policy is None else 2
        if len(parameters) < argument_start:
            raise TypeError("an authenticated HTTP handler must take request and identity inputs")
        self.path = path
        self.graph = graph
        self.dictionary_input = _http_handler_input_is_dictionary(graph._native_definition)
        self.parameters = parameters[argument_start:]
        self.signature = inspect.Signature(self.parameters)
        update_wrapper(self, graph.function)
        self.__signature__ = self.signature

    def __call__(self, *args, **kwargs):
        bound = self.signature.bind(*args, **kwargs)
        bound.apply_defaults()

        client_id = hg.service_client_id()
        requests = _http_handler_service.client_replies(client_id)
        identities = None
        rejections = None
        handler_requests = requests
        if self.policy_graphs is not None:
            request_graph, reply_graph = self.policy_graphs
            client_ids = hg.map(request_graph, requests)
            decisions = hg.map(reply_graph, client_ids)
            projected = _http_authenticate_requests(requests, decisions)
            handler_requests = hg.operators.getattr(projected, hg.Symbol("requests"))
            identities = hg.operators.getattr(projected, hg.Symbol("identities"))
            rejections = hg.operators.getattr(projected, hg.Symbol("rejections"))

        if self.dictionary_input:
            if identities is None:
                result = self.graph(handler_requests, *bound.args, **bound.kwargs)
            else:
                result = self.graph(handler_requests, identities, *bound.args, **bound.kwargs)
        else:
            mapped_positional = tuple(hg.pass_through(value) for value in bound.args)
            mapped_named = {name: hg.pass_through(value) for name, value in bound.kwargs.items()}
            if identities is None:
                result = hg.map(self.graph, handler_requests, *mapped_positional, **mapped_named)
            else:
                result = hg.map(self.graph, handler_requests, identities, *mapped_positional, **mapped_named)

        responses = _http_response_field(result)
        if rejections is not None:
            responses = _http_merge_responses(responses, rejections)
        route = hg.operators.const_(self.path)
        _http_handler_service.client_requests(client_id, route, responses)
        return result


def http_handler(path, graph, policy=None):
    """Create a route that runs a graph for HTTP requests and returns its outputs."""
    return HttpHandler(path, graph, policy)


def register_http_server(
    catalogue, host="127.0.0.1", port=0, *, certificate_chain_file=None, private_key_file=None
):
    """Register an HTTP or HTTPS server implementation in a service catalogue."""
    if (certificate_chain_file is None) != (private_key_file is None):
        raise ValueError("HTTPS requires both certificate_chain_file and private_key_file")

    @hg.graph
    def implementation():
        service_requests = _http_handler_service.implementation_requests()
        routes = hg.operators.getattr(service_requests, hg.Symbol("route"))
        responses = hg.operators.getattr(service_requests, hg.Symbol("responses"))
        websocket_service_requests = _websocket_handler_service.implementation_requests()
        websocket_routes = hg.operators.getattr(websocket_service_requests, hg.Symbol("route"))
        websocket_responses = hg.operators.getattr(websocket_service_requests, hg.Symbol("responses"))
        outputs = _http_server_impl(
            routes,
            responses,
            websocket_routes,
            websocket_responses,
            host,
            port,
            certificate_chain_file,
            private_key_file,
        )
        _http_handler_service.implementation_replies(hg.operators.getattr(outputs, hg.Symbol("requests")))
        _websocket_handler_service.implementation_replies(
            hg.operators.getattr(outputs, hg.Symbol("websocket_requests"))
        )
        _http_server_port_service.implementation_replies(hg.operators.getattr(outputs, hg.Symbol("port")))

    registration = catalogue.register_multiservice(implementation)
    registration.register_impl(_http_handler_service)
    registration.register_impl(_websocket_handler_service)
    registration.register_impl(_http_server_port_service)


def http_server():
    """Return the bound port after the server installs its handler routes."""
    return _http_server_port_service()


class WebSocketHandler:
    def __init__(self, path, graph, policy=None):
        if not isinstance(path, str) or not path.startswith("/") or any(c in path for c in "?#"):
            raise ValueError("WebSocket handler paths must be absolute paths without query or fragment")
        if not isinstance(graph, hg.GraphDefinition):
            raise TypeError("websocket_handler expects a decorated @graph definition")
        if policy is not None and not isinstance(policy, hg._ServiceDefinition):
            raise TypeError("WebSocket authentication policy must be a hgraph.service declaration")

        parameters = tuple(inspect.signature(graph.function).parameters.values())
        if not parameters:
            raise TypeError("a WebSocket handler graph must take a request input")
        first_ambient_parameter = 1 if policy is None else 2
        if len(parameters) < first_ambient_parameter:
            raise TypeError("an authenticated WebSocket handler must take request and identity inputs")

        self.path = path
        self.graph = graph
        self.policy = policy
        self.parameters = parameters[first_ambient_parameter:]
        self.signature = inspect.Signature(self.parameters)
        self.fixed_parameters = graph._fixed_parameters[first_ambient_parameter:]
        update_wrapper(self, graph.function)
        self.__signature__ = self.signature

    def __call__(self, *args, **kwargs):
        fixed_values, positional_values, named_values = hg._bind_definition_arguments(
            self.signature, self.fixed_parameters, args, kwargs
        )

        return _websocket_handler(
            self.path,
            self.graph._native_definition,
            self.policy,
            fixed_values,
            positional_values,
            named_values,
        )


def websocket_handler(path, graph, policy=None):
    """Create a WebSocket route that wires a graph's outputs to connections."""
    return WebSocketHandler(path, graph, policy)


@hg.service
def http_client(request: hg.TS[HttpClientRequest]) -> hg.TS[HttpResponse]:
    """Send each request update over HTTP and return its response."""
    raise RuntimeError("http_client is a service declaration")


@hg.service
def websocket_client(request: hg.TSB[WebSocketClientRequest]) -> hg.TSB[WebSocketClientReply]:
    """Send connection, message, and close updates over a binary WebSocket."""
    raise RuntimeError("websocket_client is a service declaration")


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

    @hg.graph
    def implementation(request: hg.TSD[int, hg.TS[HttpClientRequest]]) -> hg.TSD[int, hg.TS[HttpResponse]]:
        return _http_client_impl(request, timeout_ms, max_response_body_size)

    return implementation


def websocket_client_impl(
    handshake_timeout=timedelta(seconds=30),
    max_message_size=8 * 1024 * 1024,
    ca_file=None,
):
    """Build a graph implementation for the binary WebSocket client service."""
    if not isinstance(handshake_timeout, timedelta):
        raise TypeError("handshake_timeout must be a datetime.timedelta")
    handshake_timeout_ms = handshake_timeout // timedelta(milliseconds=1)
    if handshake_timeout_ms <= 0:
        raise ValueError("handshake_timeout must be at least one millisecond")
    if isinstance(max_message_size, bool) or not isinstance(max_message_size, int) or max_message_size <= 0:
        raise ValueError("max_message_size must be a positive integer")
    if ca_file is not None and not isinstance(ca_file, str):
        raise TypeError("ca_file must be a path string or None")

    @hg.graph
    def implementation(
        request: hg.TSD[int, hg.TSB[WebSocketClientRequest]],
    ) -> hg.TSD[int, hg.TSB[WebSocketClientReply]]:
        return _websocket_client_impl(request, handshake_timeout_ms, max_message_size, ca_file)

    return implementation


@hg.service
def client_auth_registration(matcher: hg.TS[_ClientAuthMatcher], scope: str) -> hg.TSS[str]:
    """Internal service that requests credentials for selected origins."""
    raise RuntimeError("client_auth_registration is a service declaration")


@hg.service
def client_credential_submission(credentials: hg.TSD[str, hg.TS[Auth]], scope: str) -> None:
    """Internal outputless service that submits credentials for an auth registration."""
    raise RuntimeError("client_credential_submission is a service declaration")


_register_services(
    http_client,
    websocket_client,
    _websocket_handler_service,
    client_auth_registration,
    client_credential_submission,
)


class ClientAuthRegex:
    """Match a client authentication origin with a full-match regular expression."""

    def __init__(self, pattern):
        if not isinstance(pattern, str):
            raise TypeError("client auth regular expression must be a string")
        _validate_client_auth_regex(pattern)
        self.pattern = pattern


def register_client_auth(scope, matcher, credential_graph, *args, **kwargs):
    """Register a graph that returns Auth for each origin selected by matcher."""
    if not isinstance(scope, str):
        raise TypeError("client auth scope must be a string")
    if not isinstance(credential_graph, hg.GraphDefinition):
        raise TypeError("register_client_auth expects a decorated @graph credential graph")

    if isinstance(matcher, ClientAuthRegex):
        pattern = matcher.pattern
        is_regex = True
    elif isinstance(matcher, str):
        pattern = _normalize_client_auth_origin(matcher)
        is_regex = False
    else:
        raise TypeError("client auth matcher must be an origin string or ClientAuthRegex")

    registration_id = hg.service_client_id()
    client_auth_registration.client_requests(
        registration_id,
        hg.operators.const_(_ClientAuthMatcher(pattern=pattern, regex=is_regex)),
        scope,
    )
    origins = client_auth_registration.client_replies(registration_id, scope)
    named_arguments = {name: hg.pass_through(value) for name, value in kwargs.items()}
    if "__key_arg__" in named_arguments:
        raise ValueError("__key_arg__ is reserved by register_client_auth")
    named_arguments["__key_arg__"] = 0
    credentials = hg.map(
        credential_graph,
        origins,
        *(hg.pass_through(value) for value in args),
        **named_arguments,
    )
    client_credential_submission.client_requests(registration_id, credentials, scope)


def register_client_transports(
    catalogue,
    scope,
    timeout=timedelta(seconds=30),
    max_response_body_size=8 * 1024 * 1024,
    *,
    handshake_timeout=timedelta(seconds=30),
    max_message_size=8 * 1024 * 1024,
    ca_file=None,
):
    """Register HTTP and WebSocket client transports and scoped auth services."""
    if not isinstance(catalogue, hg.ServiceCatalogue):
        raise TypeError("register_client_transports expects a ServiceCatalogue")
    if not isinstance(scope, str):
        raise TypeError("client auth scope must be a string")
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

    if not isinstance(handshake_timeout, timedelta):
        raise TypeError("handshake_timeout must be a datetime.timedelta")
    handshake_timeout_ms = handshake_timeout // timedelta(milliseconds=1)
    if handshake_timeout_ms <= 0:
        raise ValueError("handshake_timeout must be at least one millisecond")
    if isinstance(max_message_size, bool) or not isinstance(max_message_size, int) or max_message_size <= 0:
        raise ValueError("max_message_size must be a positive integer")
    if ca_file is not None and not isinstance(ca_file, str):
        raise TypeError("ca_file must be a path string or None")

    _register_client_transports(
        catalogue._native_catalogue,
        scope,
        timeout_ms,
        max_response_body_size,
        handshake_timeout_ms=handshake_timeout_ms,
        max_message_size=max_message_size,
        ca_file=ca_file,
    )

__all__ = [
    "HttpPeer",
    "HttpServerRequest",
    "HttpClientRequest",
    "HttpResponse",
    "WebSocketConnectRequest",
    "WebSocketClose",
    "WebSocketServerRequest",
    "WebSocketResponse",
    "WebSocketClientRequest",
    "WebSocketClientReply",
    "Auth",
    "AnonymousAuth",
    "BasicAuth",
    "DigestAuth",
    "NegotiateAuth",
    "NtlmAuth",
    "BearerAuth",
    "HeaderAuth",
    "ClientAuthRegex",
    "AuthDecision",
    "http_handler",
    "websocket_handler",
    "register_http_server",
    "http_server",
    "http_client",
    "http_client_impl",
    "websocket_client",
    "websocket_client_impl",
    "register_client_auth",
    "register_client_transports",
]
