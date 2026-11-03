from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import socket
import sys
from threading import Event, Thread
from datetime import timedelta

import hgraph as hg
import pytest
from hg_web import (
    Auth,
    BearerAuth,
    ClientAuthRegex,
    HttpClientRequest,
    HttpResponse,
    HttpServerRequest,
    http_client,
    http_client_impl,
    http_handler,
    http_server,
    register_client_auth,
    register_http_server,
    register_client_transports,
)


def test_http_client_service_isolated_across_repeated_runtime_runs():
    request_body = b"\x00request\xff"
    response_body = b"\x00response\xfe"
    received = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers["Content-Length"])
            received.append((self.path, self.rfile.read(length), self.headers.get_all("X-Dup")))
            self.send_response(500)
            self.send_header("X-Dup", "first")
            self.send_header("X-Dup", "second")
            self.end_headers()
            self.wfile.write(response_body)

        def log_message(self, format, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_thread = Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    replies = []
    stop_sources = []

    @hg.compute_node
    def observe(response: hg.TS[HttpResponse]) -> None:
        value = response.value
        replies.append((value.status, list(value.headers), bytes(value.body)))
        stop_sources[-1].request_stop()

    @hg.graph
    def application() -> None:
        request = hg.operators.const_(
            HttpClientRequest(
                f"http://127.0.0.1:{server.server_port}/echo",
                method="POST",
                headers=[("X-Dup", "first"), ("X-Dup", "second")],
                body=request_body,
            )
        )
        observe(http_client(request))

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(http_client, http_client_impl())
    try:
        for _ in range(2):
            stop_sources.append(hg.StopSource())
            hg.run_graph(
                application,
                catalogue=catalogue,
                engine=hg.EvaluationEngine.realtime,
                run_time=timedelta(seconds=5),
                stop_token=stop_sources[-1].get_token(),
            )
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(2)

    assert received == [("/echo", request_body, ["first", "second"])] * 2
    assert len(replies) == 2
    for status, headers, body in replies:
        assert status == 500
        assert [(name, value) for name, value in headers if name.lower() == "x-dup"] == [
            ("X-Dup", "first"),
            ("X-Dup", "second"),
        ]
        assert body == response_body


@pytest.mark.parametrize("separate_clients", [False, True])
def test_http_client_overlaps_requests_and_delivers_completion_order(separate_clients):
    fast_response_seen = Event()
    requests = []
    slow_wait_results = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            if self.path == "/slow":
                slow_wait_results.append(fast_response_seen.wait(4))
                body = b"slow"
            else:
                body = b"fast"
            self.send_response(200)
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_thread = Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    replies = []
    stop = hg.StopSource()

    @hg.queue
    def client_requests(writer: hg.Queue = None) -> hg.TS[HttpClientRequest]:
        assert writer.write(HttpClientRequest(f"http://127.0.0.1:{server.server_port}/slow"))
        assert writer.write(HttpClientRequest(f"http://127.0.0.1:{server.server_port}/fast"))

    @hg.compute_node
    def observe(response: hg.TS[HttpResponse]) -> None:
        body = bytes(response.value.body)
        replies.append(body)
        if body == b"fast":
            fast_response_seen.set()
        if len(replies) == 2:
            stop.request_stop()

    @hg.graph
    def application() -> None:
        if separate_clients:
            slow = hg.operators.const_(
                HttpClientRequest(f"http://127.0.0.1:{server.server_port}/slow")
            )
            fast = hg.operators.const_(
                HttpClientRequest(f"http://127.0.0.1:{server.server_port}/fast")
            )
            observe(http_client(slow))
            observe(http_client(fast))
        else:
            observe(http_client(client_requests()))

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(http_client, http_client_impl())
    try:
        hg.run_graph(
            application,
            catalogue=catalogue,
            engine=hg.EvaluationEngine.realtime,
            run_time=timedelta(seconds=5),
            stop_token=stop.get_token(),
        )
    finally:
        fast_response_seen.set()
        server.shutdown()
        server.server_close()
        server_thread.join(2)

    assert set(requests) == {"/slow", "/fast"}
    assert slow_wait_results == [True]
    assert replies == [b"fast", b"slow"]


def test_http_client_returns_599_for_invalid_requests_and_transport_failures():
    release_slow_request = Event()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/slow":
                release_slow_request.wait(2)
                return
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"response body exceeds three bytes")

        def log_message(self, format, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_thread = Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    refused_port = probe.getsockname()[1]

    request_values = [
        HttpClientRequest("file:///not-an-http-url"),
        HttpClientRequest("http://user:password@127.0.0.1/"),
        HttpClientRequest("http://127.0.0.1/#fragment"),
        HttpClientRequest(f"http://127.0.0.1:{server.server_port}/", headers=[("bad header", "value")]),
        HttpClientRequest(f"http://127.0.0.1:{server.server_port}/", headers=[("Host", "example.test")]),
        HttpClientRequest(f"http://127.0.0.1:{server.server_port}/", headers=[("X-Test", "safe\r\nInjected: yes")]),
        HttpClientRequest(f"http://127.0.0.1:{refused_port}/"),
        HttpClientRequest(f"http://127.0.0.1:{server.server_port}/large"),
        HttpClientRequest(f"http://127.0.0.1:{server.server_port}/slow"),
    ]
    replies = []
    stop = hg.StopSource()

    @hg.queue
    def requests(writer: hg.Queue = None) -> hg.TS[HttpClientRequest]:
        for request in request_values:
            assert writer.write(request)

    @hg.compute_node
    def observe(response: hg.TS[HttpResponse]) -> None:
        value = response.value
        replies.append((value.status, bytes(value.body)))
        if len(replies) == len(request_values):
            stop.request_stop()

    @hg.graph
    def application() -> None:
        observe(http_client(requests()))

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(
        http_client,
        http_client_impl(timeout=timedelta(milliseconds=500), max_response_body_size=3),
    )
    try:
        hg.run_graph(
            application,
            catalogue=catalogue,
            engine=hg.EvaluationEngine.realtime,
            run_time=timedelta(seconds=5),
            stop_token=stop.get_token(),
        )
    finally:
        release_slow_request.set()
        probe.close()
        server.shutdown()
        server.server_close()
        server_thread.join(2)

    assert len(replies) == len(request_values)
    assert all(status == 599 for status, _ in replies)
    bodies = [body.decode() for _, body in replies]
    assert any("scheme must be http or https" in body for body in bodies)
    assert any("credentials" in body.lower() for body in bodies), bodies
    assert any("fragments are not supported" in body for body in bodies)
    assert any("HTTP header name is not a valid token" in body for body in bodies)
    assert any("controls request routing or framing" in body for body in bodies)
    assert any("without line breaks" in body for body in bodies)
    assert any("HTTP request failed:" in body for body in bodies), bodies
    assert "HTTP request timed out" in bodies
    assert "HTTP response body exceeds configured limit" in bodies


def test_http_client_graph_stop_discards_pending_request():
    request_started = Event()
    release_request = Event()
    stop = hg.StopSource()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            request_started.set()
            stop.request_stop()
            release_request.wait(2)

        def log_message(self, format, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_thread = Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    replies = []

    @hg.compute_node
    def observe(response: hg.TS[HttpResponse]) -> None:
        replies.append(bytes(response.value.body))

    @hg.graph
    def application() -> None:
        request = hg.operators.const_(HttpClientRequest(f"http://127.0.0.1:{server.server_port}/pending"))
        observe(http_client(request))

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(http_client, http_client_impl())
    try:
        hg.run_graph(
            application,
            catalogue=catalogue,
            engine=hg.EvaluationEngine.realtime,
            run_time=timedelta(seconds=5),
            stop_token=stop.get_token(),
        )
    finally:
        release_request.set()
        server.shutdown()
        server.server_close()
        server_thread.join(2)

    assert request_started.is_set()
    assert replies == []


def test_client_transports_and_auth_registration_integrate_with_python_services():
    received_authorization = []
    stop = hg.StopSource()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            authorization = self.headers.get("Authorization", "")
            received_authorization.append(authorization)
            status = 200 if authorization == "Bearer python-token" else 401
            body = b"authorized" if status == 200 else b"unauthorized"
            self.send_response(status)
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_thread = Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    @hg.graph
    def credentials(origin: hg.TS[str]) -> hg.TS[Auth]:
        del origin
        return hg.operators.const_(BearerAuth("python-token"))

    @hg.compute_node
    def describe(response: hg.TS[HttpResponse]) -> hg.TS[str]:
        value = response.value
        stop.request_stop()
        return f"{value.status}:{bytes(value.body).decode()}"

    @hg.graph
    def application() -> hg.TS[str]:
        origin = f"http://127.0.0.1:{server.server_port}"
        register_client_auth("python-client-auth", ClientAuthRegex(r"http://127\.0\.0\.1:[0-9]+"), credentials)
        request = hg.operators.const_(HttpClientRequest(f"{origin}/private"))
        return describe(http_client(request))

    catalogue = hg.ServiceCatalogue()
    register_client_transports(catalogue, "python-client-auth", timeout=timedelta(seconds=2))
    try:
        replies = hg.run_graph(
            application,
            catalogue=catalogue,
            engine=hg.EvaluationEngine.realtime,
            run_time=timedelta(seconds=4),
            stop_token=stop.get_token(),
            capture_output=True,
        )
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(2)

    assert [value for _, value in replies.events] == ["200:authorized"]
    assert received_authorization == ["Bearer python-token"]


def test_https_client_ca_file_and_combined_transport_option(monkeypatch):
    from pathlib import Path

    test_data = Path(__file__).parent.parent / "tests" / "data"
    certificate = test_data / "test_server_cert.pem"
    private_key = test_data / "test_server_key.pem"
    wrong_ca = test_data / "test_wrong_ca.pem"

    def request_with(ca_file=None, combined=False):
        stop = hg.StopSource()
        statuses = []

        @hg.graph
        def handler(request: hg.TS[HttpServerRequest]) -> hg.TS[HttpResponse]:
            return hg.operators.sample(request, hg.operators.const_(HttpResponse(status=204)))

        route = http_handler("/tls", handler)

        @hg.compute_node
        def make_request(port: hg.TS[int]) -> hg.TS[HttpClientRequest]:
            return HttpClientRequest(f"https://localhost:{port.value}/tls")

        @hg.compute_node
        def observe(response: hg.TS[HttpResponse]) -> None:
            value = response.value
            statuses.append((value.status, bytes(value.body)))
            stop.request_stop()

        @hg.graph
        def application() -> None:
            route()
            observe(http_client(make_request(http_server())))

        catalogue = hg.ServiceCatalogue()
        register_http_server(
            catalogue,
            certificate_chain_file=str(certificate),
            private_key_file=str(private_key),
        )
        if combined:
            register_client_transports(
                catalogue,
                "python-https-ca-file",
                ca_file=str(ca_file) if ca_file is not None else None,
            )
        else:
            implementation = http_client_impl() if ca_file is None else http_client_impl(ca_file=str(ca_file))
            catalogue.register_impl(http_client, implementation)
        hg.run_graph(
            application,
            catalogue=catalogue,
            engine=hg.EvaluationEngine.realtime,
            run_time=timedelta(seconds=5),
            stop_token=stop.get_token(),
        )
        return statuses

    assert [status for status, body in request_with()] == [599]
    assert request_with(certificate)[0][0] == 204
    assert request_with(wrong_ca)[0][0] == 599
    combined = request_with(certificate, combined=True)
    assert combined[0][0] == 204, combined

    if sys.platform.startswith("linux"):
        monkeypatch.setenv("SSL_CERT_FILE", str(certificate))
        assert request_with()[0][0] == 204
        assert request_with(wrong_ca)[0][0] == 599
