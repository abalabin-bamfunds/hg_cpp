import copy
from datetime import timedelta

import hgraph as hg

from hg_web import (
    AuthDecision,
    HttpResponse,
    HttpServerRequest,
    WebSocketClientReply,
    WebSocketClientRequest,
    WebSocketClose,
    WebSocketConnectRequest,
    WebSocketResponse,
    WebSocketServerRequest,
    http_server,
    register_client_transports,
    register_http_server,
    websocket_client,
    websocket_handler,
)


@hg.schema
class User:
    name: str


def test_python_websocket_handler_and_client_roundtrip_with_policy_and_ambient_input():
    decision_type = AuthDecision[User]
    stop = hg.StopSource()
    handshake_statuses = []
    received_messages = []
    close_codes = []
    revocation_writers = []
    revocation_sent = False

    @hg.service
    def authenticate(request: hg.TS[HttpServerRequest]) -> hg.TS[decision_type]:
        raise RuntimeError("authenticate is a service declaration")

    @hg.compute_node
    def decide(request: hg.TS[HttpServerRequest], revoked: hg.TS[int]) -> hg.TS[decision_type]:
        if request.value.path == "/reject":
            return decision_type.reject(HttpResponse(status=403, body=b"denied"))
        if revoked.value:
            return decision_type.reject(HttpResponse(status=403, body=b"revoked"))
        return decision_type.accept(User("alice"))

    @hg.queue
    def revocations(writer: hg.Queue = None) -> hg.TS[int]:
        revocation_writers.append(copy.copy(writer))
        assert writer.write(0)

    @hg.graph
    def policy_graph(request: hg.TS[HttpServerRequest], revoked: hg.TS[int]) -> hg.TS[decision_type]:
        return decide(request, revoked)

    @hg.graph
    def authentication_implementation(
        request: hg.TSD[int, hg.TS[HttpServerRequest]],
    ) -> hg.TSD[int, hg.TS[decision_type]]:
        return hg.map(policy_graph, request, revocations())

    @hg.compute_node
    def handshake(identity: hg.TS[User]) -> hg.TS[HttpResponse]:
        assert identity.value.name == "alice"
        return HttpResponse(status=101)

    @hg.compute_node
    def add_context(message: hg.TS[bytes], identity: hg.TS[User], topic: hg.TS[str]) -> hg.TS[bytes]:
        return identity.value.name.encode() + b"/" + topic.value.encode() + b":" + bytes(message.value)

    @hg.graph
    def echo(
        request: hg.TSB[WebSocketServerRequest], identity: hg.TS[User], topic: hg.TS[str]
    ) -> hg.TSB[WebSocketResponse]:
        messages = hg.operators.emit(hg.operators.getattr(request, hg.symbol("messages")))
        return hg.operators.combine[hg.TSB[WebSocketResponse]](
            connect_response=handshake(identity),
            message=add_context(messages, identity, topic),
            close=hg.operators.nothing[hg.TS[WebSocketClose]](),
        )

    route = websocket_handler("/socket", echo, policy=authenticate)
    rejected_route = websocket_handler("/reject", echo, policy=authenticate)

    @hg.compute_node
    def make_request(port: hg.TS[int], path: str) -> hg.TSB[WebSocketClientRequest]:
        return {
            "connect_request": WebSocketConnectRequest(f"ws://127.0.0.1:{port.value}{path}"),
            "message": b"ping",
        }

    @hg.compute_node
    def observe(reply: hg.TSB[WebSocketClientReply]) -> None:
        nonlocal revocation_sent
        response = reply.connect_response
        if response.valid and response.modified:
            handshake_statuses.append(response.value.status)

        messages = reply.messages
        if messages.valid and messages.modified and messages.value:
            assert all(memoryview(value).readonly for value in messages.value)
            received_messages.extend(bytes(value) for value in messages.value)
            if not revocation_sent:
                assert revocation_writers[0].write(1)
                revocation_sent = True

        close = reply.close
        if close.valid and close.modified:
            close_codes.append(close.value.code)

        if 101 in handshake_statuses and 403 in handshake_statuses and received_messages and 1008 in close_codes:
            stop.request_stop()

    @hg.graph
    def application() -> None:
        route("topic")
        rejected_route("topic")
        port = http_server()
        observe(websocket_client(make_request(port, "/socket")))
        observe(websocket_client(make_request(port, "/reject")))

    catalogue = hg.ServiceCatalogue()
    catalogue.register_impl(authenticate, authentication_implementation)
    register_http_server(catalogue)
    register_client_transports(catalogue, "python-websocket")
    hg.run_graph(
        application,
        catalogue=catalogue,
        engine=hg.EvaluationEngine.realtime,
        run_time=timedelta(seconds=5),
        stop_token=stop.get_token(),
    )

    assert sorted(handshake_statuses) == [101, 403]
    assert received_messages == [b"alice/topic:ping"]
    assert close_codes == [1008]


def test_python_websocket_dictionary_handler_and_wss_ca_file():
    from pathlib import Path

    @hg.schema
    class DictionaryHandlerOutput:
        response: hg.TSD[int, hg.TSB[WebSocketResponse]]
        extra: hg.TS[str]

    certificate = Path(__file__).parent.parent / "tests" / "data" / "test_server_cert.pem"
    stop = hg.StopSource()
    handshake_statuses = []
    received_messages = []
    extra_outputs = []

    @hg.compute_node
    def add_topic(message: hg.TS[bytes], topic: hg.TS[str]) -> hg.TS[bytes]:
        return topic.value.encode() + b":" + bytes(message.value)

    @hg.graph
    def echo(
        request: hg.TSB[WebSocketServerRequest], *, topic: hg.TS[str]
    ) -> hg.TSB[WebSocketResponse]:
        messages = hg.operators.emit(hg.operators.getattr(request, hg.symbol("messages")))
        return hg.operators.combine[hg.TSB[WebSocketResponse]](
            connect_response=hg.operators.const_(HttpResponse(status=101)),
            message=add_topic(messages, topic),
            close=hg.operators.nothing[hg.TS[WebSocketClose]](),
        )

    @hg.graph
    def dictionary_handler(
        requests: hg.TSD[int, hg.TSB[WebSocketServerRequest]], *, topic: hg.TS[str]
    ) -> hg.TSB[DictionaryHandlerOutput]:
        responses = hg.map(echo, requests, topic=hg.pass_through(topic))
        return hg.operators.combine[hg.TSB[DictionaryHandlerOutput]](response=responses, extra=topic)

    route = websocket_handler("/dictionary", dictionary_handler)

    @hg.compute_node
    def make_request(port: hg.TS[int]) -> hg.TSB[WebSocketClientRequest]:
        return {
            "connect_request": WebSocketConnectRequest(f"wss://localhost:{port.value}/dictionary"),
            "message": b"ping",
        }

    @hg.compute_node
    def observe(reply: hg.TSB[WebSocketClientReply]) -> None:
        response = reply.connect_response
        if response.valid and response.modified:
            handshake_statuses.append(response.value.status)
        messages = reply.messages
        if messages.valid and messages.modified and messages.value:
            received_messages.extend(bytes(value) for value in messages.value)
            stop.request_stop()

    @hg.compute_node
    def observe_extra(value: hg.TS[str]) -> None:
        extra_outputs.append(value.value)

    @hg.graph
    def application() -> None:
        output = route(topic="dictionary")
        observe_extra(hg.operators.getattr(output, hg.symbol("extra")))
        observe(websocket_client(make_request(http_server())))

    catalogue = hg.ServiceCatalogue()
    register_http_server(
        catalogue,
        host="127.0.0.1",
        port=0,
        certificate_chain_file=str(certificate),
        private_key_file=str(certificate.with_name("test_server_key.pem")),
    )
    register_client_transports(catalogue, "python-wss", ca_file=str(certificate))
    hg.run_graph(
        application,
        catalogue=catalogue,
        engine=hg.EvaluationEngine.realtime,
        run_time=timedelta(seconds=5),
        stop_token=stop.get_token(),
    )

    assert handshake_statuses == [101]
    assert received_messages == [b"dictionary:ping"]
    assert extra_outputs == ["dictionary"]


def test_python_websocket_client_replaces_connection_and_closes_it():
    stop = hg.StopSource()
    request_writers = []
    handshake_statuses = []
    received_messages = []
    close_codes = []
    port_value = []

    @hg.queue
    def client_requests(writer: hg.Queue = None) -> hg.TSB[WebSocketClientRequest]:
        request_writers.append(copy.copy(writer))

    @hg.graph
    def echo(request: hg.TSB[WebSocketServerRequest]) -> hg.TSB[WebSocketResponse]:
        messages = hg.operators.emit(hg.operators.getattr(request, hg.symbol("messages")))
        return hg.operators.combine[hg.TSB[WebSocketResponse]](
            connect_response=hg.operators.const_(HttpResponse(status=101)),
            message=messages,
            close=hg.operators.nothing[hg.TS[WebSocketClose]](),
        )

    route = websocket_handler("/socket", echo)

    @hg.compute_node
    def connect(port: hg.TS[int]) -> None:
        port_value.append(port.value)
        request = WebSocketConnectRequest(f"ws://127.0.0.1:{port.value}/socket")
        assert request_writers[0].write({"connect_request": request, "message": b"first"})

    @hg.compute_node
    def observe(reply: hg.TSB[WebSocketClientReply]) -> None:
        response = reply.connect_response
        if response.valid and response.modified:
            handshake_statuses.append(response.value.status)

        messages = reply.messages
        if messages.valid and messages.modified and messages.value:
            assert isinstance(messages.value, tuple)
            received_messages.extend(bytes(value) for value in messages.value)
            if received_messages == [b"first"]:
                request = WebSocketConnectRequest(f"ws://127.0.0.1:{port_value[0]}/socket")
                assert request_writers[0].write({"connect_request": request, "message": b"second"})
            elif received_messages == [b"first", b"second"]:
                assert request_writers[0].write({"close": WebSocketClose(1000, "finished")})

        close = reply.close
        if close.valid and close.modified:
            close_codes.append(close.value.code)
            stop.request_stop()

    @hg.graph
    def application() -> None:
        route()
        port = http_server()
        connect(port)
        observe(websocket_client(client_requests()))

    catalogue = hg.ServiceCatalogue()
    register_http_server(catalogue)
    register_client_transports(catalogue, "python-websocket-lifecycle")
    hg.run_graph(
        application,
        catalogue=catalogue,
        engine=hg.EvaluationEngine.realtime,
        run_time=timedelta(seconds=5),
        stop_token=stop.get_token(),
    )

    assert handshake_statuses == [101, 101]
    assert received_messages == [b"first", b"second"]
    assert close_codes == [1000]


def test_python_websocket_reply_message_batch_is_an_ordered_tuple():
    batches = []

    @hg.queue(mode=hg.QueueMode.batch)
    def messages(writer: hg.Queue = None) -> hg.TS[tuple[bytes, ...]]:
        assert writer.write(b"first")
        assert writer.write(b"second")

    @hg.compute_node
    def observe(reply: hg.TSB[WebSocketClientReply]) -> None:
        values = reply.messages
        assert isinstance(values.value, tuple)
        batches.append(tuple(bytes(value) for value in values.value))

    @hg.graph
    def application() -> None:
        reply = hg.operators.combine[hg.TSB[WebSocketClientReply]](
            connect_response=hg.operators.nothing[hg.TS[HttpResponse]](),
            messages=messages(),
            close=hg.operators.nothing[hg.TS[WebSocketClose]](),
        )
        observe(reply)

    hg.run_graph(application)

    assert batches == [(b"first", b"second")]
