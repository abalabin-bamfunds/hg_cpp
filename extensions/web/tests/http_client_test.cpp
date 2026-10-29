#include <bg/graph.hpp>
#include <bg/literal/scalar_literal.hpp>
#include <bg/map.hpp>
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
#include "../src/client_runtime.hpp"

#include "../src/http_header.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
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

struct ObservedResponse {
    i64 status{};
    HeaderValues headers;
    std::vector<std::byte> body;
};

struct ClientRuntimeSource {
    Scalar<Opaque<HttpClientOptions>> options;
    State<ClientRuntime *> runtime_state;
    Out<TSB<bg::web::details::ClientOutput>> output;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;

    void start(EvaluationContext *context) const {
        auto output_queue = context->runtime_graph().make_queue(context->node_index());
        auto runtime = std::make_unique<ClientRuntime>(options.value(), std::move(output_queue));
        output->runtime.set(runtime.get());
        *runtime_state = runtime.release();
    }

    void stop() const noexcept { delete std::exchange(*runtime_state, nullptr); }
    void finalize() const noexcept { delete std::exchange(*runtime_state, nullptr); }
};

const auto client_runtime_source = node<ClientRuntimeSource>();

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

TEST_CASE("HTTP client runtime sends typed Basic authentication", "[feature:http][feature:auth]") {
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue);

    auto memory = allocator{};
    const auto basic = bundle<BasicAuth>(memory, "username"_n = std::string{"alice"},
                                         "password"_n = std::string{"secret"});
    const auto auth = ScalarValue<Bundle<Auth>>{basic.view()};
    auto observed_authorization = std::string{};
    auto response = std::optional<ObservedResponse>{};
    auto submitted = false;
    constexpr auto client_id = i64{42};
    const auto handler = node([&](In<TS<Bundle<HttpServerRequest>>> request,
                                  Out<TS<Bundle<HttpResponse>>> output) {
        const auto headers = scalar_cast<List<Header>>(request.value()[4]);
        for (auto index = std::size_t{}; index < headers.size(); ++index) {
            const auto header = headers[index];
            if (scalar_cast<string>(header.template get<0>()).value() == "Authorization") {
                observed_authorization = scalar_cast<string>(header.template get<1>()).value();
            }
        }
        const auto value = make_response(204, {});
        output.set_value(value.view());
    });
    const auto route = http_handler("/client-auth", handler);
    const auto submit = node([&](In<TS<i64>> port, In<TS<Opaque<ClientRuntime *>>, passive> runtime) {
        if (submitted) return;
        const auto request = make_request(
            "GET", "http://127.0.0.1:" + std::to_string(*port) + "/client-auth");
        runtime.value()->request(client_id, 1, ScalarValue<Bundle<HttpClientRequest>>{request.view()},
                                 std::optional<ScalarValue<Bundle<Auth>>>{
                                     ScalarValue<Bundle<Auth>>{auth.view()}});
        submitted = true;
    });
    const auto collect = node([&](In<TSD<i64, TS<Bundle<HttpResponse>>>> replies) {
        if (!replies.valid() || !replies.modified() || !replies.contains(client_id)) return;
        const auto value = replies[client_id];
        if (!value.valid() || !value.modified()) return;
        response.emplace(observe(value.value()));
        stop.request_stop();
    });

    run_graph(
        [&] {
            (void)route();
            const auto port = http_server();
            const auto source = client_runtime_source(HttpClientOptions{});
            submit(port, operators::getattr(source, util::symbol{"runtime"}));
            collect(operators::getattr(source, util::symbol{"replies"}));
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{4},
                        .stop_token = stop.get_token()});

    REQUIRE(submitted);
    REQUIRE(response.has_value());
    CHECK(response->status == 204);
    CHECK(observed_authorization == "Basic YWxpY2U6c2VjcmV0");
}

TEST_CASE("HTTP client runtime rejects a base-only authentication value", "[feature:http][feature:auth]") {
    auto memory = allocator{};
    const auto auth = bundle<Auth>(memory);
    auto stop = std::stop_source{};
    auto response = std::optional<ObservedResponse>{};
    auto submitted = false;
    constexpr auto client_id = i64{42};
    const auto submit = node([&](In<TS<Opaque<ClientRuntime *>>> runtime) {
        if (submitted) return;
        const auto request = make_request("GET", "http://127.0.0.1/");
        runtime.value()->request(client_id, 1, ScalarValue<Bundle<HttpClientRequest>>{request.view()},
                                 std::optional<ScalarValue<Bundle<Auth>>>{
                                     ScalarValue<Bundle<Auth>>{auth.view()}});
        submitted = true;
    });
    const auto collect = node([&](In<TSD<i64, TS<Bundle<HttpResponse>>>> replies) {
        if (!replies.valid() || !replies.modified() || !replies.contains(client_id)) return;
        const auto value = replies[client_id];
        if (!value.valid() || !value.modified()) return;
        response.emplace(observe(value.value()));
        stop.request_stop();
    });

    run_graph(
        [&] {
            const auto source = client_runtime_source(HttpClientOptions{});
            submit(operators::getattr(source, util::symbol{"runtime"}));
            collect(operators::getattr(source, util::symbol{"replies"}));
        },
        GraphRunOptions{.engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{3},
                        .stop_token = stop.get_token()});

    REQUIRE(submitted);
    REQUIRE(response.has_value());
    CHECK(response->status == 599);
    CHECK(std::string{reinterpret_cast<const char *>(response->body.data()), response->body.size()} ==
          "HTTP authentication schema is not supported");
}

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

TEST_CASE("HTTP client continues after a 599 response", "[feature:http][feature:service][feature:realtime]") {
    const auto large_body = std::array{std::byte{'x'}, std::byte{'y'}};
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(http_client, http_client_impl({.max_response_body_size = 1}),
                            std::span<const WiringArgument>{});
    register_http_server(catalogue);

    auto request_writer = Queue{};
    const auto requests = queue<TS<Bundle<HttpClientRequest>>>(start = [&](Queue queue) { request_writer = queue; });
    const auto large_response = node([&](In<TS<Bundle<HttpServerRequest>>>, Out<TS<Bundle<HttpResponse>>> response) {
        const auto value = make_response(200, {}, large_body);
        response.set_value(value.view());
    });
    const auto small_response = node([](In<TS<Bundle<HttpServerRequest>>>, Out<TS<Bundle<HttpResponse>>> response) {
        const auto value = make_response(200, {});
        response.set_value(value.view());
    });
    const auto large_route = http_handler("/large", large_response);
    const auto small_route = http_handler("/small", small_response);

    auto responses = std::vector<ObservedResponse>{};
    auto first_write_accepted = false;
    auto second_write_accepted = false;
    auto bound_port_value = i64{};
    run_graph(
        [&] {
            (void)large_route();
            (void)small_route();
            const auto port = http_server();
            const auto replies = http_client(requests());
            (void)node([&](In<TS<i64>> bound_port) {
                if (!bound_port.valid() || !bound_port.modified()) return;
                bound_port_value = *bound_port;
                const auto request = make_request(
                    "GET", "http://127.0.0.1:" + std::to_string(bound_port_value) + "/large");
                first_write_accepted = request_writer.write(literal::TimeSeriesLiteral(request.view()));
            })(port);
            (void)node([&](In<TS<Bundle<HttpResponse>>> reply) {
                if (!reply.valid() || !reply.modified()) return;
                responses.push_back(observe(reply.value()));
                if (responses.size() == 1) {
                    const auto request = make_request(
                        "GET", "http://127.0.0.1:" + std::to_string(bound_port_value) + "/small");
                    second_write_accepted = request_writer.write(literal::TimeSeriesLiteral(request.view()));
                    return;
                }
                stop.request_stop();
            })(replies);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{4},
                        .stop_token = stop.get_token()});

    CHECK(first_write_accepted);
    CHECK(second_write_accepted);
    REQUIRE(responses.size() == 2);
    CHECK(responses[0].status == 599);
    CHECK(responses[1].status == 200);
    CHECK(responses[1].body.empty());
}

TEST_CASE("HTTP client returns queue replies for mapped callers",
          "[feature:http][feature:service][feature:map][feature:realtime]"
          "[feature:lifecycle][transition:invalidation][transition:revalidation]") {
    const auto client_id = service_client_id_t{42};
    auto stop = std::stop_source{};
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(http_client, http_client_impl(), std::span<const WiringArgument>{});

    auto request_writer = Queue{};
    const auto requests = queue<TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>>(
        start = [client_id, &request_writer](Queue queue) {
            request_writer = queue;
            const auto request = make_request("GET", "file:///invalid");
            if (!queue.write(literal::TimeSeriesLiteral{{client_id, request.view()}})) {
                throw std::runtime_error{"could not queue the mapped HTTP client request"};
            }
        });
    const auto client = graph([](wp<TS<Bundle<HttpClientRequest>>> request) { return http_client(request); });

    auto observed = std::vector<ObservedResponse>{};
    auto activation = 0;
    run_graph(
        [&] {
            const auto replies = map(client, requests());
            (void)node([&](In<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>> values) {
                if (!values.valid() || !values.modified() || !values.contains(client_id)) return;
                const auto response = values[client_id];
                if (activation == 1 && !response.valid()) {
                    activation = 2;
                    const auto replacement = make_request("BAD METHOD", "http://127.0.0.1/");
                    if (!request_writer.write(literal::TimeSeriesLiteral{{client_id, replacement.view()}})) {
                        throw std::runtime_error{"could not queue the reactivated HTTP client request"};
                    }
                    return;
                }
                if (!response.valid() || !response.modified()) return;
                observed.push_back(observe(response.value()));
                if (activation == 0) {
                    activation = 1;
                    if (!request_writer.write(literal::TimeSeriesLiteral{{client_id, literal::INVALID}})) {
                        throw std::runtime_error{"could not withdraw the HTTP client request"};
                    }
                    return;
                }
                if (activation == 2) stop.request_stop();
            })(replies);
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{3},
                        .stop_token = stop.get_token()});

    REQUIRE(activation == 2);
    REQUIRE(observed.size() == 2);
    CHECK(observed[0].status == 599);
    CHECK(std::string{reinterpret_cast<const char *>(observed[0].body.data()), observed[0].body.size()} ==
          "URL scheme must be http or https");
    CHECK(observed[1].status == 599);
    CHECK(std::string{reinterpret_cast<const char *>(observed[1].body.data()), observed[1].body.size()} ==
          "HTTP method is not a valid token");
}
