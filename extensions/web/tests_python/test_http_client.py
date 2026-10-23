from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import socket
from threading import Event, Thread
from datetime import timedelta

import bg
import pytest
from bg_web import HttpClientRequest, HttpResponse, http_client, http_client_impl


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

    @bg.compute_node
    def observe(response: bg.TS[HttpResponse]) -> None:
        value = response.value
        replies.append((value.status, list(value.headers), bytes(value.body)))
        stop_sources[-1].request_stop()

    @bg.graph
    def application() -> None:
        request = bg.operators.const_(
            HttpClientRequest(
                f"http://127.0.0.1:{server.server_port}/echo",
                method="POST",
                headers=[("X-Dup", "first"), ("X-Dup", "second")],
                body=request_body,
            )
        )
        observe(http_client(request))

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(http_client, http_client_impl())
    try:
        for _ in range(2):
            stop_sources.append(bg.StopSource())
            bg.run_graph(
                application,
                catalogue=catalogue,
                engine=bg.EvaluationEngine.realtime,
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
    stop = bg.StopSource()

    @bg.queue
    def client_requests(writer: bg.Queue = None) -> bg.TS[HttpClientRequest]:
        assert writer.write(HttpClientRequest(f"http://127.0.0.1:{server.server_port}/slow"))
        assert writer.write(HttpClientRequest(f"http://127.0.0.1:{server.server_port}/fast"))

    @bg.compute_node
    def observe(response: bg.TS[HttpResponse]) -> None:
        body = bytes(response.value.body)
        replies.append(body)
        if body == b"fast":
            fast_response_seen.set()
        if len(replies) == 2:
            stop.request_stop()

    @bg.graph
    def application() -> None:
        if separate_clients:
            slow = bg.operators.const_(
                HttpClientRequest(f"http://127.0.0.1:{server.server_port}/slow")
            )
            fast = bg.operators.const_(
                HttpClientRequest(f"http://127.0.0.1:{server.server_port}/fast")
            )
            observe(http_client(slow))
            observe(http_client(fast))
        else:
            observe(http_client(client_requests()))

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(http_client, http_client_impl())
    try:
        bg.run_graph(
            application,
            catalogue=catalogue,
            engine=bg.EvaluationEngine.realtime,
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
    stop = bg.StopSource()

    @bg.queue
    def requests(writer: bg.Queue = None) -> bg.TS[HttpClientRequest]:
        for request in request_values:
            assert writer.write(request)

    @bg.compute_node
    def observe(response: bg.TS[HttpResponse]) -> None:
        value = response.value
        replies.append((value.status, bytes(value.body)))
        if len(replies) == len(request_values):
            stop.request_stop()

    @bg.graph
    def application() -> None:
        observe(http_client(requests()))

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(
        http_client,
        http_client_impl(timeout=timedelta(milliseconds=500), max_response_body_size=3),
    )
    try:
        bg.run_graph(
            application,
            catalogue=catalogue,
            engine=bg.EvaluationEngine.realtime,
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
    assert any("credentials" in body.lower() for body in bodies)
    assert any("fragments are not supported" in body for body in bodies)
    assert any("HTTP header name is not a valid token" in body for body in bodies)
    assert any("controls request routing or framing" in body for body in bodies)
    assert any("without line breaks" in body for body in bodies)
    assert any("HTTP request failed:" in body for body in bodies)
    assert "HTTP request timed out" in bodies
    assert "HTTP response body exceeds configured limit" in bodies


def test_http_client_graph_stop_discards_pending_request():
    request_started = Event()
    release_request = Event()
    stop = bg.StopSource()

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

    @bg.compute_node
    def observe(response: bg.TS[HttpResponse]) -> None:
        replies.append(bytes(response.value.body))

    @bg.graph
    def application() -> None:
        request = bg.operators.const_(HttpClientRequest(f"http://127.0.0.1:{server.server_port}/pending"))
        observe(http_client(request))

    catalogue = bg.ServiceCatalogue()
    catalogue.register_impl(http_client, http_client_impl())
    try:
        bg.run_graph(
            application,
            catalogue=catalogue,
            engine=bg.EvaluationEngine.realtime,
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
