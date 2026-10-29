#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/queue_node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/http_server.hpp>
#include <bg/web/websocket_client.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::web;

namespace {

using Header = Tuple<string, string>;
using HeaderValues = std::vector<std::pair<std::string, std::string>>;

ScalarValue<Bundle<WebSocketConnectRequest>> make_connect_request(std::string url) {
    const auto headers = ScalarValue<List<Header>>{HeaderValues{{"Sec-WebSocket-Protocol", "chat"}}};
    auto memory = allocator{};
    return bundle<WebSocketConnectRequest>(memory, "url"_n = url, "headers"_n = headers.view());
}

ScalarValue<Bundle<WebSocketClose>> make_close(i64 code, std::string_view reason) {
    auto memory = allocator{};
    return bundle<WebSocketClose>(memory, "code"_n = code, "reason"_n = reason);
}

ScalarValue<Bundle<HttpResponse>> make_http_response(i64 status, HeaderValues headers, std::string_view body) {
    const auto header_values = ScalarValue<List<Header>>{headers};
    const auto body_bytes = std::span<const std::byte>{reinterpret_cast<const std::byte *>(body.data()), body.size()};
    const auto body_value = ScalarValue<bytes>{body_bytes};
    auto memory = allocator{};
    return bundle<HttpResponse>(memory, "status"_n = status, "headers"_n = header_values.view(),
                                "body"_n = body_value.view());
}

std::vector<std::byte> copy_bytes(Scalar<bytes> value) {
    const auto bytes = value.value();
    return {bytes.begin(), bytes.end()};
}

} // namespace

TEST_CASE("WebSocket client exchanges binary messages before close and returns handshake metadata",
          "[feature:websocket][feature:service][feature:realtime]") {
    const auto first_message = std::array{std::byte{0}, std::byte{0x62}, std::byte{0xff}, std::byte{0}};
    const auto finished_bytes = std::array{std::byte{0x7f}};
    auto stop = std::stop_source{};
    auto server_received_final_message = false;
    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue);
    catalogue.register_impl(websocket_client, websocket_client_impl(), std::span<const WiringArgument>{});

    auto request_writer = Queue{};
    const auto requests = queue<TSB<BinaryWebSocketClientRequest>>(
        start = [&](Queue queue) { request_writer = std::move(queue); });
    const auto echo = graph([finished_bytes, &server_received_final_message](
                                wp<TSB<WebSocketServerRequest<bytes>::schema>> request) {
        const auto messages = operators::getattr(request, util::symbol{"messages"});
        (void)node([&server_received_final_message, finished_bytes](In<TS<List<bytes>>> values) {
            if (!values.valid() || !values.modified()) return;
            const auto messages = values.value_view();
            for (auto index = std::size_t{}; index < messages.size(); ++index) {
                if (copy_bytes(messages[index]) ==
                    std::vector<std::byte>{finished_bytes.begin(), finished_bytes.end()}) {
                    server_received_final_message = true;
                }
            }
        })(messages);
        const auto finished_value = ScalarValue<bytes>{std::span<const std::byte>{finished_bytes}};
        const auto message = operators::emit(messages);
        const auto finished = operators::eq(message, operators::const_(finished_value.view()));
        const auto no_message = operators::nothing[type<TS<bytes>>]();
        const auto response_message = operators::if_then_else(finished, no_message, message);
        const auto close_value = make_close(1000, "finished");
        const auto close = operators::const_(close_value.view());
        const auto no_close = operators::nothing[type<TS<Bundle<WebSocketClose>>>]();

        const auto response_headers = ScalarValue<List<Header>>{HeaderValues{{"Sec-WebSocket-Protocol", "chat"}}};
        const auto empty_body = ScalarValue<bytes>{std::span<const std::byte>{}};
        auto memory = allocator{};
        const auto handshake = bundle<HttpResponse>(memory, "status"_n = i64{101},
                                                    "headers"_n = response_headers.view(),
                                                    "body"_n = empty_body.view());
        const auto response = operators::const_(handshake.view());
        return bundle<WebSocketResponse<bytes>::schema>(
            "connect_response"_n = response, "message"_n = response_message,
            "close"_n = operators::if_then_else(finished, close, no_close));
    });
    const auto route = websocket_handler("/socket", echo);

    auto handshake_status = i64{};
    auto selected_protocol = std::string{};
    auto echoes = std::vector<std::vector<std::byte>>{};
    auto close_code = i64{};
    auto close_count = std::size_t{};
    auto bound_port_value = i64{};
    auto connected = false;
    auto empty_sent = false;
    auto final_sent = false;
    auto reconnect_sent = false;

    const auto send_message = [&](std::span<const std::byte> message, bool include_close) {
        if (include_close) {
            const auto close = make_close(1000, "client complete");
            if (!request_writer.write(literal::TimeSeriesLiteral{{"message", literal::ScalarLiteral(message)},
                                                                 {"close", close.view()}})) {
                throw std::runtime_error{"could not queue a WebSocket client update"};
            }
            return;
        }
        if (!request_writer.write(literal::TimeSeriesLiteral{{"message", literal::ScalarLiteral(message)}})) {
            throw std::runtime_error{"could not queue a WebSocket client update"};
        }
    };

    run_graph(
        [&] {
            (void)route();
            const auto port = http_server();
            (void)node([&](In<TS<i64>> bound_port) {
                bound_port_value = *bound_port;
                const auto request = make_connect_request("ws://127.0.0.1:" + std::to_string(*bound_port) + "/socket");
                if (!request_writer.write(literal::TimeSeriesLiteral{{"connect_request", request.view()},
                                                                     {"message", literal::ScalarLiteral(
                                                                                     std::span<const std::byte>{
                                                                                         first_message})}})) {
                    throw std::runtime_error{"could not queue the WebSocket connection request"};
                }
            })(port);

            const auto reply = websocket_client(requests());
            (void)node([&](In<TSB<BinaryWebSocketClientReply>> value) {
                if (value->connect_response.valid() && value->connect_response.modified()) {
                    const auto response = value->connect_response.value();
                    handshake_status = scalar_cast<i64>(response[0]).value();
                    const auto headers = scalar_cast<List<Header>>(response[1]);
                    for (auto index = std::size_t{}; index < headers.size(); ++index) {
                        const auto header = headers[index];
                        if (scalar_cast<string>(header.template get<0>()).value() == "Sec-WebSocket-Protocol") {
                            selected_protocol = std::string{scalar_cast<string>(header.template get<1>()).value()};
                        }
                    }
                    connected = handshake_status == 101;
                    if (reconnect_sent && connected) {
                        const auto close = make_close(1000, "reconnected");
                        if (!request_writer.write(literal::TimeSeriesLiteral{{"close", close.view()}})) {
                            throw std::runtime_error{"could not queue the replacement connection close"};
                        }
                    }
                }

                if (value->messages.valid() && value->messages.modified()) {
                    const auto messages = value->messages.value_view();
                    for (auto index = std::size_t{}; index < messages.size(); ++index) {
                        echoes.push_back(copy_bytes(messages[index]));
                    }
                }

                if (connected && !empty_sent && !echoes.empty()) {
                    empty_sent = true;
                    send_message(std::span<const std::byte>{}, false);
                }
                if (empty_sent && !final_sent && echoes.size() >= 2) {
                    final_sent = true;
                    send_message(std::span<const std::byte>{finished_bytes}, true);
                }

                if (value->close.valid() && value->close.modified()) {
                    close_code = scalar_cast<i64>(value->close.value()[0]).value();
                    ++close_count;
                }
                if (close_count == 1 && !reconnect_sent) {
                    reconnect_sent = true;
                    const auto request = make_connect_request("ws://127.0.0.1:" +
                                                              std::to_string(bound_port_value) + "/socket");
                    if (!request_writer.write(literal::TimeSeriesLiteral{{"connect_request", request.view()}})) {
                        throw std::runtime_error{"could not queue the replacement WebSocket connection"};
                    }
                }
                if (close_count == 2) stop.request_stop();
            })(reply);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{5},
                        .stop_token = stop.get_token()});

    CHECK(handshake_status == 101);
    CHECK(selected_protocol == "chat");
    CHECK(echoes == std::vector<std::vector<std::byte>>{
                        {std::byte{0}, std::byte{0x62}, std::byte{0xff}, std::byte{0}}, {}});
    CHECK(server_received_final_message);
    CHECK(close_code == 1000);
    CHECK(reconnect_sent);
    CHECK(close_count == 2);
}

TEST_CASE("WebSocket client preserves a rejected handshake response",
          "[feature:websocket][feature:service][failure]") {
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue);
    catalogue.register_impl(websocket_client, websocket_client_impl(), std::span<const WiringArgument>{});
    auto request_writer = Queue{};
    const auto requests = queue<TSB<BinaryWebSocketClientRequest>>(
        start = [&](Queue queue) { request_writer = std::move(queue); });
    const auto reject = graph([](wp<TSB<WebSocketServerRequest<bytes>::schema>>) {
        const auto response = make_http_response(403, {{"X-Rejection", "policy"}}, "denied");
        const auto no_message = operators::nothing[type<TS<bytes>>]();
        const auto no_close = operators::nothing[type<TS<Bundle<WebSocketClose>>>]();
        return bundle<WebSocketResponse<bytes>::schema>("connect_response"_n = operators::const_(response.view()),
                                                       "message"_n = no_message, "close"_n = no_close);
    });
    const auto route = websocket_handler("/reject", reject);

    auto status = i64{};
    auto response_body = std::vector<std::byte>{};
    auto rejection_header = std::string{};
    run_graph(
        [&] {
            (void)route();
            const auto port = http_server();
            (void)node([&](In<TS<i64>> bound_port) {
                const auto request = make_connect_request("ws://127.0.0.1:" + std::to_string(*bound_port) + "/reject");
                if (!request_writer.write(literal::TimeSeriesLiteral{{"connect_request", request.view()}})) {
                    throw std::runtime_error{"could not queue the rejected WebSocket connection"};
                }
            })(port);

            const auto reply = websocket_client(requests());
            (void)node([&](In<TSB<BinaryWebSocketClientReply>> value) {
                if (!value->connect_response.modified()) return;
                const auto response = value->connect_response.value();
                status = scalar_cast<i64>(response[0]).value();
                response_body = copy_bytes(scalar_cast<bytes>(response[2]));
                const auto headers = scalar_cast<List<Header>>(response[1]);
                for (auto index = std::size_t{}; index < headers.size(); ++index) {
                    const auto header = headers[index];
                    if (scalar_cast<string>(header.template get<0>()).value() == "X-Rejection") {
                        rejection_header = std::string{scalar_cast<string>(header.template get<1>()).value()};
                    }
                }
                stop.request_stop();
            })(reply);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{4},
                        .stop_token = stop.get_token()});

    CHECK(status == 403);
    CHECK(response_body == std::vector<std::byte>{std::byte{'d'}, std::byte{'e'}, std::byte{'n'},
                                                  std::byte{'i'}, std::byte{'e'}, std::byte{'d'}});
    CHECK(rejection_header == "policy");
}

TEST_CASE("WebSocket close cancels a pending handshake before validating the close code",
          "[feature:websocket][feature:service][transition:lifecycle]") {
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(websocket_client, websocket_client_impl(), std::span<const WiringArgument>{});
    const auto requests = queue<TSB<BinaryWebSocketClientRequest>>(
        start = [](Queue queue) {
            const auto request = make_connect_request("ws://127.0.0.1:9/socket");
            const auto close = make_close(1005, "invalid while pending");
            if (!queue.write(literal::TimeSeriesLiteral{{"connect_request", request.view()},
                                                       {"close", close.view()}})) {
                throw std::runtime_error{"could not queue the pending WebSocket close"};
            }
        });

    auto status = i64{};
    auto body = std::string{};
    run_graph(
        [&] {
            const auto reply = websocket_client(requests());
            (void)node([&](In<TSB<BinaryWebSocketClientReply>> value) {
                if (!value->connect_response.modified()) return;
                const auto response = value->connect_response.value();
                status = scalar_cast<i64>(response[0]).value();
                const auto bytes = scalar_cast<bg::bytes>(response[2]).value();
                body.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                stop.request_stop();
            })(reply);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{3},
                        .stop_token = stop.get_token()});

    CHECK(status == 599);
    CHECK(body == "WebSocket handshake canceled by close request");
}

TEST_CASE("WebSocket client restarts a removed branch without a stale handshake response",
          "[feature:websocket][feature:service][feature:lifecycle][transition:removal][transition:restart]") {
    const auto client_id = service_client_id_t{42};
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(websocket_client, websocket_client_impl(), std::span<const WiringArgument>{});

    auto request_writer = Queue{};
    const auto requests = queue<TSD<service_client_id_t, TSB<BinaryWebSocketClientRequest>>>(start = [&](Queue queue) {
        request_writer = queue;
        const auto request = make_connect_request("http://first.invalid/socket");
        if (!queue.write(literal::TimeSeriesLiteral{{client_id, { {"connect_request", request.view()} }}})) {
            throw std::runtime_error{"could not queue the first WebSocket branch request"};
        }
    });
    const auto client = graph([](wp<TSB<BinaryWebSocketClientRequest>> request) { return websocket_client(request); });

    auto response_statuses = std::vector<i64>{};
    auto response_bodies = std::vector<std::string>{};
    auto first_reply_seen = false;
    auto branch_removed = false;
    auto second_request_sent = false;
    run_graph(
        [&] {
            const auto replies = map(client, requests());
            (void)node([&](In<TSD<service_client_id_t, TSB<BinaryWebSocketClientReply>>> values) {
                if (!values.valid() || !values.modified()) return;
                if (!values.contains(client_id)) {
                    if (!first_reply_seen || second_request_sent) return;
                    branch_removed = true;
                    second_request_sent = true;
                    const auto request = make_connect_request("ws://bad%0dhost.invalid/socket");
                    if (!request_writer.write(
                            literal::TimeSeriesLiteral{{client_id, {{"connect_request", request.view()}}}})) {
                        throw std::runtime_error{"could not queue the replacement WebSocket branch request"};
                    }
                    return;
                }

                const auto reply = values[client_id];
                if (!reply.valid() || !reply->connect_response.valid() ||
                    !reply->connect_response.modified()) {
                    return;
                }
                const auto response = reply->connect_response.value();
                response_statuses.push_back(scalar_cast<i64>(response[0]).value());
                const auto body = scalar_cast<bytes>(response[2]).value();
                response_bodies.emplace_back(reinterpret_cast<const char *>(body.data()), body.size());
                if (!first_reply_seen) {
                    first_reply_seen = true;
                    if (!request_writer.write(literal::TimeSeriesLiteral{{client_id, literal::REMOVE}})) {
                        throw std::runtime_error{"could not remove the first WebSocket client branch"};
                    }
                    return;
                }
                stop.request_stop();
            })(replies);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{5},
                        .stop_token = stop.get_token()});

    CHECK(response_statuses == std::vector<i64>{599, 599});
    CHECK(response_bodies == std::vector<std::string>{"WebSocket URL scheme must be ws",
                                                       "WebSocket URL host contains a control character"});
    CHECK(branch_removed);
    CHECK(second_request_sent);
}

TEST_CASE("WebSocket client rejects duplicate subprotocol offers",
          "[feature:websocket][feature:service][failure]") {
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(websocket_client, websocket_client_impl(), std::span<const WiringArgument>{});
    const auto requests = queue<TSB<BinaryWebSocketClientRequest>>(
        start = [](Queue queue) {
            const auto headers = ScalarValue<List<Header>>{HeaderValues{{"Sec-WebSocket-Protocol", "chat, chat"}}};
            auto memory = allocator{};
            const auto request = bundle<WebSocketConnectRequest>(
                memory, "url"_n = std::string{"ws://127.0.0.1:9/socket"}, "headers"_n = headers.view());
            if (!queue.write(literal::TimeSeriesLiteral{{"connect_request", request.view()}})) {
                throw std::runtime_error{"could not queue the WebSocket connection request"};
            }
        });

    auto status = i64{};
    auto body = std::string{};
    run_graph(
        [&] {
            const auto reply = websocket_client(requests());
            (void)node([&](In<TSB<BinaryWebSocketClientReply>> value) {
                if (!value->connect_response.modified()) return;
                const auto response = value->connect_response.value();
                status = scalar_cast<i64>(response[0]).value();
                const auto bytes = scalar_cast<bg::bytes>(response[2]).value();
                body.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                stop.request_stop();
            })(reply);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{3},
                        .stop_token = stop.get_token()});

    CHECK(status == 599);
    CHECK(body == "WebSocket subprotocol offer is invalid");
}

TEST_CASE("WebSocket client rejects control characters decoded from the URL host",
          "[feature:websocket][feature:service][failure]") {
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(websocket_client, websocket_client_impl(), std::span<const WiringArgument>{});
    const auto requests = queue<TSB<BinaryWebSocketClientRequest>>(
        start = [](Queue queue) {
            const auto request = make_connect_request("ws://bad%0dhost.invalid/socket");
            if (!queue.write(literal::TimeSeriesLiteral{{"connect_request", request.view()}})) {
                throw std::runtime_error{"could not queue the WebSocket connection request"};
            }
        });

    auto status = i64{};
    auto body = std::string{};
    run_graph(
        [&] {
            const auto reply = websocket_client(requests());
            (void)node([&](In<TSB<BinaryWebSocketClientReply>> value) {
                if (!value->connect_response.modified()) return;
                const auto response = value->connect_response.value();
                status = scalar_cast<i64>(response[0]).value();
                const auto bytes = scalar_cast<bg::bytes>(response[2]).value();
                body.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                stop.request_stop();
            })(reply);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{3},
                        .stop_token = stop.get_token()});

    CHECK(status == 599);
    CHECK(body == "WebSocket URL host contains a control character");
}
