import http.client
import os
import subprocess
import sys
import threading
from datetime import timedelta

import bg
from bg_web import HttpResponse, HttpServerRequest, http_handler, http_server, register_http_server


def test_bg_web_imports_before_bg():
    subprocess.run([sys.executable, "-c", "import bg_web; import bg"], check=True,
                   env=os.environ.copy(), timeout=5)


def test_http_server_routes_binary_request_and_returns_native_bytes():
    request_bodies = []
    replies = []
    errors = []
    worker = None
    stop = bg.StopSource()

    @bg.compute_node
    def echo_node(request: bg.TS[HttpServerRequest]) -> bg.TS[HttpResponse]:
        value = request.value
        assert value.method == "POST"
        if value.path == "/echo":
            assert list(value.query) == [("name", "a b")]
        else:
            assert value.path == "/dictionary"
            assert list(value.query) == []
        assert value.peer.port > 0
        assert memoryview(value.body).readonly
        request_bodies.append(bytes(value.body))
        return HttpResponse(status=200, headers=[], body=value.body)

    @bg.graph
    def echo(request: bg.TS[HttpServerRequest]) -> bg.TS[HttpResponse]:
        return echo_node(request)

    @bg.graph
    def dictionary_echo(request: bg.TS[HttpServerRequest]) -> bg.TS[HttpResponse]:
        return echo_node(request)

    @bg.graph
    def dictionary_handler(
        requests: bg.TSD[int, bg.TS[HttpServerRequest]],
    ) -> bg.TSD[int, bg.TS[HttpResponse]]:
        return bg.map(dictionary_echo, requests)

    echo_route = http_handler("/echo", echo)
    dictionary_route = http_handler("/dictionary", dictionary_handler)

    @bg.compute_node
    def start_client(port: bg.TS[int]) -> bg.TS[int]:
        nonlocal worker
        bound_port = int(port.value)

        def request():
            try:
                connection = http.client.HTTPConnection("127.0.0.1", bound_port, timeout=3)
                connection.request("POST", "/echo?name=a+b", body=b"\x00binary\xff")
                response = connection.getresponse()
                replies.append((response.status, response.read()))
                connection.request("POST", "/dictionary", body=b"TSD response")
                response = connection.getresponse()
                replies.append((response.status, response.read()))
                connection.close()
            except BaseException as error:
                errors.append(error)
            finally:
                stop.request_stop()

        worker = threading.Thread(target=request)
        worker.start()
        return bound_port

    @bg.graph
    def application() -> bg.TS[int]:
        echo_route()
        dictionary_route()
        return start_client(http_server())

    catalogue = bg.ServiceCatalogue()
    register_http_server(catalogue, host="127.0.0.1", port=0)
    bg.run_graph(application, catalogue=catalogue, engine=bg.EvaluationEngine.realtime,
                 run_time=timedelta(seconds=5), stop_token=stop.get_token())
    if worker is not None:
        worker.join(3)

    assert worker is not None and not worker.is_alive()
    assert not errors, repr(errors)
    assert request_bodies == [b"\x00binary\xff", b"TSD response"]
    assert replies == [(200, b"\x00binary\xff"), (200, b"TSD response")]
