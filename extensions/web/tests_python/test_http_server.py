import http.client
import os
import pathlib
import ssl
import subprocess
import sys
import threading
from datetime import timedelta

import hgraph as hg
from hg_web import AuthDecision, HttpResponse, HttpServerRequest, http_handler, http_server, register_http_server
import pytest


def test_hg_web_imports_before_hgraph():
    dll_directory = os.environ.get("HG_TEST_DLL_DIR")
    dll_setup = (
        f"import os; _dll_directory = os.add_dll_directory({dll_directory!r}); "
        if os.name == "nt" and dll_directory
        else ""
    )
    subprocess.run([sys.executable, "-c", dll_setup + "import hg_web; import hgraph"], check=True,
                   env=os.environ.copy(), timeout=5)


def test_http_server_routes_binary_request_and_returns_native_bytes():
    request_bodies = []
    replies = []
    errors = []
    worker = None
    stop = hg.StopSource()

    @hg.compute_node
    def echo_node(request: hg.TS[HttpServerRequest]) -> hg.TS[HttpResponse]:
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

    @hg.graph
    def echo(request: hg.TS[HttpServerRequest]) -> hg.TS[HttpResponse]:
        return echo_node(request)

    @hg.graph
    def dictionary_echo(request: hg.TS[HttpServerRequest]) -> hg.TS[HttpResponse]:
        return echo_node(request)

    @hg.graph
    def dictionary_handler(
        requests: hg.TSD[int, hg.TS[HttpServerRequest]],
    ) -> hg.TSD[int, hg.TS[HttpResponse]]:
        return hg.map(dictionary_echo, requests)

    echo_route = http_handler("/echo", echo)
    dictionary_route = http_handler("/dictionary", dictionary_handler)

    @hg.compute_node
    def start_client(port: hg.TS[int]) -> hg.TS[int]:
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

    @hg.graph
    def application() -> hg.TS[int]:
        echo_route()
        dictionary_route()
        return start_client(http_server())

    catalogue = hg.ServiceCatalogue()
    register_http_server(catalogue, host="127.0.0.1", port=0)
    hg.run_graph(application, catalogue=catalogue, engine=hg.EvaluationEngine.realtime,
                 run_time=timedelta(seconds=5), stop_token=stop.get_token())
    if worker is not None:
        worker.join(3)

    assert worker is not None and not worker.is_alive()
    assert not errors, repr(errors)
    assert request_bodies == [b"\x00binary\xff", b"TSD response"]
    assert replies == [(200, b"\x00binary\xff"), (200, b"TSD response")]


def test_https_server_uses_configured_certificate():
    test_data = pathlib.Path(__file__).parent.parent / "tests" / "data"
    certificate = test_data / "test_server_cert.pem"
    private_key = test_data / "test_server_key.pem"
    replies = []
    errors = []
    worker = None
    stop = hg.StopSource()

    @hg.compute_node
    def echo(request: hg.TS[HttpServerRequest]) -> hg.TS[HttpResponse]:
        return HttpResponse(status=200, body=request.value.body)

    @hg.graph
    def echo_graph(request: hg.TS[HttpServerRequest]) -> hg.TS[HttpResponse]:
        return echo(request)

    route = http_handler("/secure", echo_graph)

    @hg.compute_node
    def start_client(port: hg.TS[int]) -> hg.TS[int]:
        nonlocal worker
        bound_port = int(port.value)

        def request():
            try:
                context = ssl.create_default_context(cafile=str(certificate))
                connection = http.client.HTTPSConnection("localhost", bound_port, context=context, timeout=3)
                connection.request("POST", "/secure", body=b"tls")
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

    @hg.graph
    def application() -> hg.TS[int]:
        route()
        return start_client(http_server())

    catalogue = hg.ServiceCatalogue()
    register_http_server(
        catalogue,
        host="127.0.0.1",
        port=0,
        certificate_chain_file=str(certificate),
        private_key_file=str(private_key),
    )
    hg.run_graph(application, catalogue=catalogue, engine=hg.EvaluationEngine.realtime,
                 run_time=timedelta(seconds=5), stop_token=stop.get_token())
    if worker is not None:
        worker.join(3)

    assert worker is not None and not worker.is_alive()
    assert not errors, repr(errors)
    assert replies == [(200, b"tls")]


def test_https_server_requires_certificate_and_private_key_together():
    with pytest.raises(ValueError, match="certificate_chain_file and private_key_file"):
        register_http_server(hg.ServiceCatalogue(), certificate_chain_file="server-chain.pem")


def test_http_handler_authentication_routes_identity_and_rejection():
    @hg.schema
    class User:
        name: str

    decision_type = AuthDecision[User]

    @hg.service
    def authenticate(request: hg.TS[HttpServerRequest]) -> hg.TS[decision_type]:
        raise RuntimeError("authentication is a service declaration")

    @hg.compute_node
    def decide(request: hg.TS[HttpServerRequest]) -> hg.TS[decision_type]:
        authorized = any(name.lower() == "authorization" and value == "allow" for name, value in request.value.headers)
        if authorized:
            return decision_type.accept(User("alice"))
        return decision_type.reject(HttpResponse(status=401, body=b"denied"))

    @hg.graph
    def policy_graph(request: hg.TS[HttpServerRequest]) -> hg.TS[decision_type]:
        return decide(request)

    @hg.graph
    def policy_implementation(
        request: hg.TSD[int, hg.TS[HttpServerRequest]],
    ) -> hg.TSD[int, hg.TS[decision_type]]:
        return hg.map(policy_graph, request)

    handler_calls = []

    @hg.compute_node
    def greet(request: hg.TS[HttpServerRequest], user: hg.TS[User]) -> hg.TS[HttpResponse]:
        handler_calls.append(user.value.name)
        return HttpResponse(status=200, body=user.value.name.encode())

    @hg.graph
    def authenticated_handler(
        request: hg.TS[HttpServerRequest], user: hg.TS[User]
    ) -> hg.TS[HttpResponse]:
        return greet(request, user)

    route = http_handler("/auth", authenticated_handler, policy=authenticate)
    replies = []
    errors = []
    worker = None
    stop = hg.StopSource()

    @hg.compute_node
    def start_client(port: hg.TS[int]) -> hg.TS[int]:
        nonlocal worker
        bound_port = int(port.value)

        def request():
            try:
                connection = http.client.HTTPConnection("127.0.0.1", bound_port, timeout=3)
                connection.request("GET", "/auth", headers={"Authorization": "allow"})
                response = connection.getresponse()
                replies.append((response.status, response.read()))
                connection.request("GET", "/auth", headers={"Authorization": "deny"})
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

    @hg.graph
    def application() -> hg.TS[int]:
        route()
        return start_client(http_server())

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(authenticate, policy_implementation)
    register_http_server(catalogue, host="127.0.0.1", port=0)
    hg.run_graph(application, catalogue=catalogue, engine=hg.EvaluationEngine.realtime,
                 run_time=timedelta(seconds=5), stop_token=stop.get_token())
    if worker is not None:
        worker.join(3)

    assert worker is not None and not worker.is_alive()
    assert not errors, repr(errors)
    assert replies == [(200, b"alice"), (401, b"denied")]
    assert handler_calls == ["alice"]
