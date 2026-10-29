#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/details/engine_wakeup.hpp>
#include <bg/literal/scalar_literal.hpp>
#include <bg/node.hpp>
#include <bg/queue_node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/web/http_client.hpp>
#include <bg/web/http_server.hpp>
#include <bg/web/auth.hpp>

#include "../src/client_output.hpp"
#include "../src/http_header.hpp"

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

ScalarValue<List<Header>> make_headers(const HeaderValues &headers) {
    auto values = std::vector<ScalarValue<Header>>{};
    auto views = std::vector<Scalar<Header>>{};
    values.reserve(headers.size());
    views.reserve(headers.size());
    for (const auto &[name, value] : headers) {
        values.push_back(ScalarValue<Header>::from_literal(literal::ScalarLiteral{name, value}));
        views.push_back(values.back().view());
    }
    return ScalarValue<List<Header>>{views};
}

ScalarValue<Bundle<HttpClientRequest>> make_request(std::string method, std::string url, HeaderValues headers = {},
                                                   std::span<const std::byte> body = {}) {
    auto method_value = ScalarValue<string>{method};
    auto url_value = ScalarValue<string>{url};
    auto headers_value = make_headers(headers);
    auto body_value = ScalarValue<bytes>{body};
    auto memory = allocator{};
    return bundle<HttpClientRequest>(memory, "method"_n = method_value.view(), "url"_n = url_value.view(),
                                     "headers"_n = headers_value.view(), "body"_n = body_value.view());
}

ScalarValue<Bundle<HttpResponse>> make_response(i64 status, HeaderValues headers, std::span<const std::byte> body = {}) {
    auto status_value = ScalarValue<i64>{status};
    auto headers_value = make_headers(headers);
    auto body_value = ScalarValue<bytes>{body};
    auto memory = allocator{};
    return bundle<HttpResponse>(memory, "status"_n = status_value.view(), "headers"_n = headers_value.view(),
                                "body"_n = body_value.view());
}

ScalarValue<Bundle<bg::web::details::ClientReply>> make_client_reply(i64 client_sequence,
                                                                     Scalar<Bundle<HttpResponse>> response) {
    auto client_sequence_value = ScalarValue<i64>{client_sequence};
    auto memory = allocator{};
    return bundle<bg::web::details::ClientReply>(memory, "client_sequence"_n = client_sequence_value.view(),
                                                 "response"_n = response);
}

struct ObservedResponse {
    i64 status{};
    HeaderValues headers;
    std::vector<std::byte> body;
};

ObservedResponse observe(Scalar<Bundle<HttpResponse>> response) {
    auto result = ObservedResponse{};
    result.status = scalar_cast<i64>(response[0]).value();
    const auto headers = scalar_cast<List<Header>>(response[1]);
    for (auto index = std::size_t{}; index < headers.size(); ++index) {
        const auto header = headers[index];
        result.headers.emplace_back(std::string{scalar_cast<string>(header.template get<0>()).value()},
                                    std::string{scalar_cast<string>(header.template get<1>()).value()});
    }
    const auto body = scalar_cast<bytes>(response[2]).value();
    result.body.assign(body.begin(), body.end());
    return result;
}

} // namespace

TEST_CASE("Latin-1 HTTP header values convert to UTF-8", "[feature:http]") {
    auto latin1 = std::string{};
    auto expected = std::string{};
    latin1.reserve(256);
    expected.reserve(384);
    for (auto byte = 0; byte <= 0xff; ++byte) {
        const auto character = static_cast<unsigned char>(byte);
        latin1.push_back(static_cast<char>(character));
        if (character < 0x80) {
            expected.push_back(static_cast<char>(character));
            continue;
        }
        expected.push_back(static_cast<char>(0xc0 | (character >> 6)));
        expected.push_back(static_cast<char>(0x80 | (character & 0x3f)));
    }

    CHECK(bg::web::details::latin1_header_value_as_utf8(latin1) == expected);
    auto converted = latin1;
    bg::web::details::convert_latin1_header_value_as_utf8(converted);
    CHECK(converted == expected);
}

TEST_CASE("Latin-1 HTTP header conversion preserves ASCII and empty values", "[feature:http]") {
    auto empty = std::string{};
    const auto empty_data = empty.data();
    const auto empty_capacity = empty.capacity();
    bg::web::details::convert_latin1_header_value_as_utf8(empty);
    CHECK(empty.empty());
    CHECK(empty.data() == empty_data);
    CHECK(empty.capacity() == empty_capacity);

    auto ascii = std::string{"plain ASCII header value"};
    const auto ascii_data = ascii.data();
    const auto ascii_capacity = ascii.capacity();
    bg::web::details::convert_latin1_header_value_as_utf8(ascii);
    CHECK(ascii == "plain ASCII header value");
    CHECK(ascii.data() == ascii_data);
    CHECK(ascii.capacity() == ascii_capacity);
}

TEST_CASE("Latin-1 HTTP header conversion preserves mixed ASCII and non-ASCII bytes", "[feature:http]") {
    const auto latin1 = std::string{static_cast<char>(0x80)} + "A" + static_cast<char>(0xff);
    const auto expected = std::string{"\xc2\x80" "A" "\xc3\xbf"};

    CHECK(bg::web::details::latin1_header_value_as_utf8(latin1) == expected);
    auto converted = latin1;
    bg::web::details::convert_latin1_header_value_as_utf8(converted);
    CHECK(converted == expected);
}

TEST_CASE("HTTP auth decisions contain exactly one identity or rejection",
          "[feature:http][feature:scalar]") {
    auto memory = allocator{};
    const auto identity = ScalarValue<i64>{42};
    const auto accepted = auth::accept<i64>(memory, identity.view());

    CHECK(accepted.view().present(0));
    CHECK_FALSE(accepted.view().present(1));
    CHECK(scalar_cast<i64>(accepted.view()[0]).value() == 42);

    const auto response = make_response(401, {});
    const auto rejected = auth::reject<i64>(memory, response.view());

    CHECK_FALSE(rejected.view().present(0));
    CHECK(rejected.view().present(1));
    CHECK(scalar_cast<i64>(scalar_cast<Bundle<HttpResponse>>(rejected.view()[1])[0]).value() == 401);
}

TEST_CASE("HTTP client preserves binary payloads, ordered headers, and sequential connection reuse",
          "[feature:http][feature:service][feature:realtime]") {
    const auto request_body = std::array{std::byte{0}, std::byte{'x'}, std::byte{0xff}};
    const auto response_body = std::array{std::byte{'r'}, std::byte{0}, std::byte{0xfe}};
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(http_client, http_client_impl(), std::span<const WiringArgument>{});
    register_http_server(catalogue);

    auto writer = Queue{};
    auto first_write_accepted = false;
    auto observed_requests = 0;
    auto bound_port_value = i64{};
    auto second_write_accepted = false;
    auto request_peer_ports = std::vector<i64>{};
    auto request_headers = HeaderValues{};
    auto received_request_body = std::vector<std::byte>{};
    const auto handler = node([&](In<TS<Bundle<HttpServerRequest>>> request, Out<TS<Bundle<HttpResponse>>> response) {
        const auto value = request.value();
        const auto peer = scalar_cast<Bundle<HttpPeer>>(value[6]);
        request_peer_ports.push_back(scalar_cast<i64>(peer[1]).value());
        if (observed_requests++ == 0) {
            const auto fields = scalar_cast<List<Header>>(value[4]);
            for (auto index = std::size_t{}; index < fields.size(); ++index) {
                const auto field = fields[index];
                const auto name = std::string{scalar_cast<string>(field.template get<0>()).value()};
                if (name == "X-Dup" || name == "x-dup" || name == "X-Empty") {
                    request_headers.emplace_back(
                        name, std::string{scalar_cast<string>(field.template get<1>()).value()});
                }
            }
            const auto body = scalar_cast<bytes>(value[5]).value();
            received_request_body.assign(body.begin(), body.end());
            const auto response_value =
                make_response(500, {{"X-Dup", "first"}, {"X-Dup", "second"}}, response_body);
            response.set_value(response_value.view());
        } else {
            const auto response_value = make_response(204, {});
            response.set_value(response_value.view());
        }
    });

    const auto client_requests = queue<TS<Bundle<HttpClientRequest>>>(start = [&](Queue queue) {
        writer = queue;
    });
    const auto first_route = http_handler("/first", handler);
    const auto second_route = http_handler("/second", handler);

    auto responses = std::vector<ObservedResponse>{};
    run_graph(
        [&] {
            (void)first_route();
            (void)second_route();
            const auto port = http_server();
            const auto response = http_client(client_requests());
            (void)node([&](In<TS<i64>> bound_port) {
                if (!bound_port.valid() || !bound_port.modified()) return;
                bound_port_value = *bound_port;
                const auto request = make_request(
                    "POST", "http://127.0.0.1:" + std::to_string(*bound_port) + "/first",
                    {{"X-Dup", "first"}, {"X-Empty", ""}, {"x-dup", "second"}}, request_body);
                first_write_accepted = writer.write(literal::TimeSeriesLiteral(request.view()));
            })(port);
            (void)node([&](In<TS<Bundle<HttpResponse>>> reply) {
                if (!reply.valid() || !reply.modified()) return;
                auto observed = observe(reply.value());
                if (observed.status == 204) {
                    responses.push_back(std::move(observed));
                    stop.request_stop();
                    return;
                }
                responses.push_back(std::move(observed));
                if (responses.size() == 1) {
                    const auto request = make_request(
                        "GET", "http://127.0.0.1:" + std::to_string(bound_port_value) + "/second");
                    second_write_accepted = writer.write(literal::TimeSeriesLiteral(request.view()));
                }
            })(response);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{4},
                        .stop_token = stop.get_token()});

    CHECK(first_write_accepted);
    CHECK(second_write_accepted);
    REQUIRE(responses.size() == 2);
    CHECK(responses[0].status == 500);
    CHECK(responses[1].status == 204);
    CHECK(observed_requests == 2);
    REQUIRE(request_peer_ports.size() == 2);
    CHECK(request_peer_ports[0] == request_peer_ports[1]);
    CHECK(request_headers == HeaderValues{{"X-Dup", "first"}, {"X-Empty", ""}, {"x-dup", "second"}});
    CHECK(received_request_body == std::vector<std::byte>{request_body.begin(), request_body.end()});
    auto duplicate_response_headers = HeaderValues{};
    for (const auto &header : responses[0].headers) {
        if (header.first == "X-Dup") duplicate_response_headers.push_back(header);
    }
    CHECK(duplicate_response_headers == HeaderValues{{"X-Dup", "first"}, {"X-Dup", "second"}});
    CHECK(responses[0].body == std::vector<std::byte>{response_body.begin(), response_body.end()});
    CHECK(responses[1].body.empty());
}

TEST_CASE("HTTP client dispatch rejects replies from a removed client sequence",
          "[feature:http][feature:service][feature:lifecycle][transition:removal][queue]") {
    const auto requests = queue<TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>>(start = [](Queue) {});
    const auto implementation = http_client_impl();
    auto request_point = WiringPoint{};
    auto response_point = WiringPoint{};
    auto observed_statuses = std::vector<i64>{};
    auto client_present = false;

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        request_point = requests();
        const auto output = (*implementation)(request_point);
        REQUIRE(output.has_value());
        response_point = *output;
        (void)node([&](In<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>> replies) {
            if (!replies.valid() || !replies.modified()) return;
            client_present = replies.contains(service_client_id_t{42});
            const auto observe_reply = [&](const auto &client_id) {
                const auto reply = replies[client_id];
                if (reply.valid() && reply.modified()) {
                    observed_statuses.push_back(scalar_cast<i64>(reply.value()[0]).value());
                }
            };
            replies.for_each_modified(observe_reply);
        })(response_point);
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto source_index = compiled.node_count();
    for (auto index = std::size_t{}; index < compiled.node_count(); ++index) {
        const auto &node = compiled.node(index);
        if (node.has_output_runtime_type() &&
            &node.output_runtime_type().type() == &TSB<bg::web::details::ClientOutput>::type()) {
            source_index = index;
            break;
        }
    }
    REQUIRE(source_index < compiled.node_count());

    auto global_state = GlobalState{};
    auto wakeup = bg::details::EngineWakeup{};
    auto runtime = RuntimeGraph{compiled, global_state};
    runtime.bind_queue_wakeup(wakeup);
    runtime.start(MIN_DT + std::chrono::microseconds{1});
    auto request_queue = runtime.make_queue(wiring.output_node_index(request_point));
    auto source_queue = runtime.make_queue(source_index);
    const auto wait_for_committed_queue = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!runtime.has_committed_queue_messages() && std::chrono::steady_clock::now() < deadline) {
            (void)wakeup.wait_until(deadline);
        }
        return runtime.has_committed_queue_messages();
    };
    const auto cycle = [&](i64 microseconds) { runtime.evaluate(MIN_DT + std::chrono::microseconds{microseconds}); };
    const auto reply_client_sequence = [&] {
        const auto source = runtime.node(source_index);
        const auto input = In<TS_T>{source.output_last_updated(), source.output_value(),
                                    compiled.node(source_index).output_runtime_type()};
        const auto output = input.as<TSB<bg::web::details::ClientOutput>>();
        const auto reply = output->replies[service_client_id_t{42}];
        REQUIRE(reply.valid());
        return scalar_cast<i64>(reply.value()[0]).value();
    };

    const auto first_request = make_request("GET", "file:///invalid");
    REQUIRE(request_queue.write(literal::TimeSeriesLiteral{{service_client_id_t{42}, first_request.view()}}));
    cycle(2);
    REQUIRE(wait_for_committed_queue());
    cycle(3);
    CHECK(observed_statuses == std::vector<i64>{599});
    const auto first_client_sequence = reply_client_sequence();

    REQUIRE(request_queue.write(literal::TimeSeriesLiteral{{service_client_id_t{42}, literal::REMOVE}}));
    cycle(4);
    REQUIRE(wait_for_committed_queue());
    cycle(5);
    CHECK_FALSE(client_present);

    const auto stale_response = make_response(200, {});
    const auto stale_reply = make_client_reply(first_client_sequence, stale_response.view());
    REQUIRE(source_queue.write(literal::TimeSeriesLiteral{
        {util::symbol{"replies"}, {{service_client_id_t{42}, stale_reply.view()}}}}));
    cycle(6);
    CHECK_FALSE(client_present);
    CHECK(observed_statuses == std::vector<i64>{599});

    const auto second_request = make_request("GET", "file:///invalid");
    REQUIRE(request_queue.write(literal::TimeSeriesLiteral{{service_client_id_t{42}, second_request.view()}}));
    cycle(7);
    CHECK(client_present);
    CHECK(observed_statuses == std::vector<i64>{599});
    REQUIRE(wait_for_committed_queue());
    cycle(8);
    CHECK(observed_statuses == std::vector<i64>{599, 599});
    const auto second_client_sequence = reply_client_sequence();
    CHECK(second_client_sequence != first_client_sequence);
    CHECK(client_present);

    const auto stale_response_after_readd = make_response(202, {});
    const auto stale_reply_after_readd = make_client_reply(first_client_sequence, stale_response_after_readd.view());
    REQUIRE(source_queue.write(literal::TimeSeriesLiteral{
        {util::symbol{"replies"}, {{service_client_id_t{42}, stale_reply_after_readd.view()}}}}));
    cycle(9);
    CHECK(observed_statuses == std::vector<i64>{599, 599});

    const auto current_response = make_response(201, {});
    const auto current_reply = make_client_reply(second_client_sequence, current_response.view());
    REQUIRE(source_queue.write(literal::TimeSeriesLiteral{
        {util::symbol{"replies"}, {{service_client_id_t{42}, current_reply.view()}}}}));
    cycle(10);
    CHECK(observed_statuses == std::vector<i64>{599, 599, 201});
    runtime.stop();
}
