#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/literal/scalar_literal.hpp>
#include <bg/map.hpp>
#include <bg/map_argument.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/queue.hpp>
#include <bg/realtime_evaluation_engine.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/http_server.hpp>
#include <bg/web/auth.hpp>
#include <bg/wall_clock.hpp>

#include "../src/http_server_internal.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace bg;
using namespace bg::literals;
using namespace bg::web;

namespace {

template <typename Handler> struct Route {
    Route(std::string path, Handler handler)
        : handler{std::make_shared<HttpHandler>(http_handler(std::move(path), std::move(handler)))} {}

    std::shared_ptr<HttpHandler> handler;
};

struct ServerTest {
    std::stop_source stop;

    template <typename Wire, typename Client>
        requires std::invocable<Wire>
    void run(Wire wire, Client client_function) {
        run_with_services(std::move(wire), std::move(client_function), [](ServiceCatalogue &) {});
    }

    template <typename Wire, typename Client, typename Configure>
        requires std::invocable<Wire> && std::invocable<Configure, ServiceCatalogue &>
    void run_with_services(Wire wire, Client client_function, Configure configure) {
        auto catalogue = ServiceCatalogue{};
        register_http_server(catalogue);
        std::invoke(configure, catalogue);
        run_graph(
            [&] {
                std::invoke(wire);
                const auto port = http_server();
                (void)node([&](In<TS<i64>> bound_port) {
                    client = std::jthread{[&, client_function = std::move(client_function),
                                           port = static_cast<std::uint16_t>(*bound_port)]() mutable {
                        try {
                            client_function(port);
                        } catch (...) {
                            client_failure = std::current_exception();
                            stop.request_stop();
                        }
                    }};
                })(port);
            },
            GraphRunOptions{.services = &catalogue,
                            .engine = EvaluationEngine::realtime,
                            .run_time = std::chrono::seconds{5},
                            .stop_token = stop.get_token()});

        if (client.joinable()) client.join();
        if (client_failure) std::rethrow_exception(client_failure);
    }

    template <typename Handler, typename Client>
    void run(std::initializer_list<Route<Handler>> routes, Client client_function) {
        run([routes] {
            for (const auto &route : routes) (void)(*route.handler)();
        }, std::move(client_function));
    }

  private:
    std::jthread client;
    std::exception_ptr client_failure;
};

template <template <typename, auto...> typename Field> struct RichHandlerOutput {
    Field<TS<Bundle<HttpResponse>>> response;
    Field<TS<i64>> extra;
};

struct RawHttpResponse {
    unsigned status{};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

ScalarValue<Bundle<HttpResponse>> make_http_response(i64 status, std::string_view body = {}) {
    using Header = Tuple<string, string>;
    const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
    const auto body_bytes = std::span<const std::byte>{reinterpret_cast<const std::byte *>(body.data()), body.size()};
    const auto body_value = ScalarValue<bytes>{body_bytes};
    auto memory = allocator{};
    return bundle<HttpResponse>(memory, "status"_n = status, "headers"_n = headers.view(),
                                "body"_n = body_value.view());
}

RawHttpResponse read_response(boost::asio::ip::tcp::socket &socket, boost::asio::streambuf &buffer,
                              bool head_request = false) {
    namespace asio = boost::asio;
    namespace http = boost::beast::http;
    asio::read_until(socket, buffer, "\r\n\r\n");

    const auto buffered = std::string{asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data())};
    const auto header_end = buffered.find("\r\n\r\n");
    auto response = RawHttpResponse{};
    const auto first_line_end = buffered.find("\r\n");
    auto line_start = first_line_end + 2;
    while (line_start < header_end) {
        const auto line_end = buffered.find("\r\n", line_start);
        const auto colon = buffered.find(':', line_start);
        if (colon == std::string::npos || colon >= line_end) {
            throw std::runtime_error{"invalid HTTP response header"};
        }
        auto value_start = colon + 1;
        while (value_start < line_end && buffered[value_start] == ' ') ++value_start;
        const auto name = buffered.substr(line_start, colon - line_start);
        const auto value = buffered.substr(value_start, line_end - value_start);
        response.headers.emplace_back(name, value);
        line_start = line_end + 2;
    }

    auto parser = http::response_parser<http::string_body>{};
    parser.skip(head_request);
    boost::beast::error_code error;
    const auto consumed = parser.put(buffer.data(), error);
    buffer.consume(consumed);
    if (error && error != http::error::need_more) {
        throw boost::system::system_error{error};
    }
    if (!parser.is_done()) http::read(socket, buffer, parser);
    auto message = parser.release();
    response.status = message.result_int();
    response.body = std::move(message.body());
    return response;
}

const auto echo_handler = node([](In<TS<Bundle<HttpServerRequest>>> request,
                                  Out<TS<Bundle<HttpResponse>>> response) {
    using Header = Tuple<string, string>;
    const auto empty_headers = std::vector<Scalar<Header>>{};
    const auto headers = ScalarValue<List<Header>>{empty_headers};
    auto reply = bundle<HttpResponse>("status"_n = i64{200}, "headers"_n = headers.view(),
                                      "body"_n = request.value()[5]);
    response.set_value(reply.view());
});

const auto auth_policy = service<auto(TS<Bundle<HttpServerRequest>>) -> TS<AuthDecision<i64>>>{
    "http_auth_policy", {"request"}};

struct AuthPolicy {
    inline static int evaluations{};
};

const auto auth_policy_node = node([](In<TS<Bundle<HttpServerRequest>>> request,
                                      Out<TS<AuthDecision<i64>>> decision) {
    ++AuthPolicy::evaluations;
    const auto path = scalar_cast<string>(request.value()[2]).value();
    if (path == "/auth/rejected") {
        const auto response = make_http_response(401);
        const auto result = auth::reject<i64>(response.view());
        decision.set_value(result.view());
        return;
    }

    const auto identity = ScalarValue<i64>{73};
    const auto result = auth::accept<i64>(identity.view());
    decision.set_value(result.view());
});

const auto auth_policy_implementation = graph(
    [](wp<TSD<i64, TS<Bundle<HttpServerRequest>>>, "request"_n> request)
        -> wp<TSD<i64, TS<AuthDecision<i64>>>> {
        const auto arguments = std::array{MapArgument{WiringArgument{request}}};
        const auto result = map(auth_policy_node, std::span<const MapArgument>{arguments});
        return result.as<TSD<i64, TS<AuthDecision<i64>>>>();
    });

struct AuthenticatedHandler {
    In<TS<Bundle<HttpServerRequest>>> request;
    In<TS<i64>> identity;
    Out<TS<Bundle<HttpResponse>>> response;

    void start() const { ++starts; }

    void evaluate() const {
        ++evaluations;
        const auto result = make_http_response(200, "identity=" + std::to_string(identity.value()));
        response.set_value(result.view());
    }

    inline static int starts{};
    inline static int evaluations{};
};

TEST_CASE("HTTP handlers accept ambient inputs and preserve mapped extra outputs",
          "[feature:http][feature:nested-graph][shape:nested]") {
    using Header = Tuple<string, string>;

    auto extra_values = std::vector<i64>{};
    const auto respond = node([](In<TS<Bundle<HttpServerRequest>>> request, In<TS<i64>> extra,
                                 Out<TS<Bundle<HttpResponse>>> response) {
        const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
        const auto path = scalar_cast<string>(request.value()[2]).value();
        const auto body = std::to_string(extra.value()) + std::string{path};
        const auto body_bytes = std::span<const std::byte>{reinterpret_cast<const std::byte *>(body.data()),
                                                            body.size()};
        const auto body_value = ScalarValue<bytes>{body_bytes};
        const auto result = bundle<HttpResponse>("status"_n = i64{200}, "headers"_n = headers.view(),
                                                 "body"_n = body_value.view());
        response.set_value(result.view());
    });
    const auto handler_graph = graph(
        "name"_n = "ambient_http_handler",
        [&](wp<TS<Bundle<HttpServerRequest>>> request, wp<TS<i64>> extra) {
            const auto response = respond(request, extra);
            return bundle<RichHandlerOutput>("response"_n = response, "extra"_n = extra);
        });
    const auto handler = http_handler("/rich", handler_graph);
    auto server = ServerTest{};
    auto response_body = std::string{};
    auto status = 0U;

    server.run(
        [&] {
            const auto result = handler(operators::const_(i64{42}));
            const auto project_extra = GraphTemplate{
                Signature{util::symbol{"project_extra"},
                          {{util::symbol{"bundle"}, &TSB<RichHandlerOutput>::type()}}, TS<i64>::type()},
                [](wp<> bundle) { return operators::getattr(bundle, util::symbol{"extra"}); }};
            const auto arguments = std::array{MapArgument{WiringArgument{result}}};
            const auto extra = map(project_extra, std::span<const MapArgument>{arguments});
            (void)node([&](In<TSD<i64, TS<i64>>> values) {
                const auto collect = [&](const Scalar<i64> &key) { extra_values.push_back(values[key].value()); };
                values.for_each_added(collect);
                values.for_each_modified(collect);
            })(extra);
        },
        [&](std::uint16_t port) {
            namespace asio = boost::asio;
            namespace beast = boost::beast;
            namespace http = beast::http;
            using tcp = asio::ip::tcp;

            auto context = asio::io_context{};
            auto socket = tcp::socket{context};
            socket.connect({asio::ip::make_address("127.0.0.1"), port});
            auto request = http::request<http::empty_body>{http::verb::get, "/rich", 11};
            request.set(http::field::host, "localhost");
            http::write(socket, request);
            auto buffer = beast::flat_buffer{};
            auto reply = http::response<http::string_body>{};
            http::read(socket, buffer, reply);
            status = reply.result_int();
            response_body = std::move(reply.body());
            server.stop.request_stop();
        });

    CHECK(status == 200);
    CHECK(response_body == "42/rich");
    CHECK(extra_values == std::vector<i64>{42});
}

TEST_CASE("HTTP authentication passes accepted identities and returns rejections directly",
          "[feature:http][feature:service][feature:authentication]") {
    AuthenticatedHandler::starts = 0;
    AuthenticatedHandler::evaluations = 0;
    AuthPolicy::evaluations = 0;
    const auto handler = node<AuthenticatedHandler>();
    auto server = ServerTest{};
    auto accepted_status = 0U;
    auto accepted_body = std::string{};
    auto rejected_status = 0U;
    auto rejected_body = std::string{};
    const auto accepted_route = http_handler("/auth/accepted", handler, auth_policy);
    const auto rejected_route = http_handler("/auth/rejected", handler, auth_policy);

    server.run_with_services(
        [&] {
            (void)accepted_route();
            (void)rejected_route();
        },
        [&](std::uint16_t port) {
            namespace asio = boost::asio;
            namespace beast = boost::beast;
            namespace http = beast::http;
            using tcp = asio::ip::tcp;

            auto context = asio::io_context{};
            auto socket = tcp::socket{context};
            socket.connect({asio::ip::make_address("127.0.0.1"), port});
            auto buffer = beast::flat_buffer{};

            const auto request = [&](std::string_view path) {
                auto message = http::request<http::empty_body>{http::verb::get, path, 11};
                message.set(http::field::host, "localhost");
                http::write(socket, message);
                auto reply = http::response<http::string_body>{};
                http::read(socket, buffer, reply);
                return reply;
            };

            auto accepted = request("/auth/accepted");
            accepted_status = accepted.result_int();
            accepted_body = std::move(accepted.body());
            auto rejected = request("/auth/rejected");
            rejected_status = rejected.result_int();
            rejected_body = std::move(rejected.body());
            server.stop.request_stop();
        },
        [](ServiceCatalogue &catalogue) { catalogue.register_impl(auth_policy, auth_policy_implementation); });

    CHECK(accepted_status == 200);
    CHECK(accepted_body == "identity=73");
    CHECK(rejected_status == 401);
    CHECK(rejected_body.empty());
    CHECK(AuthenticatedHandler::starts == 1);
    CHECK(AuthenticatedHandler::evaluations == 1);
    CHECK(AuthPolicy::evaluations == 2);
}

TEST_CASE("HTTP authentication keeps pending keys out of handlers and removes stale keys",
          "[feature:http][feature:service][feature:validity][series:tsd][transition:removal]") {
    using Header = Tuple<string, string>;
    auto memory = allocator{};
    const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
    const auto body = ScalarValue<bytes>{std::span<const std::byte>{}};
    const auto peer = bundle<HttpPeer>(memory, "address"_n = std::string{"127.0.0.1"}, "port"_n = i64{1});
    const auto request = bundle<HttpServerRequest>(memory, "method"_n = std::string{"GET"},
                                                   "target"_n = std::string{"/auth"},
                                                   "path"_n = std::string{"/auth"}, "query"_n = headers.view(),
                                                   "headers"_n = headers.view(), "body"_n = body.view(),
                                                   "peer"_n = peer.view());
    const auto identity_1 = ScalarValue<i64>{10};
    const auto identity_2 = ScalarValue<i64>{20};
    const auto identity_3 = ScalarValue<i64>{30};
    const auto decision_1 = auth::accept<i64>(memory, identity_1.view());
    const auto decision_2 = auth::accept<i64>(memory, identity_2.view());
    const auto decision_3 = auth::accept<i64>(memory, identity_3.view());

    const auto request_source = node([&](In<TS<i64>> cycle,
                                         Out<TSD<i64, TS<Bundle<HttpServerRequest>>>> output) {
        if (cycle.value() == 1) {
            output[1].set_value(request.view());
            output[2].set_value(request.view());
            output[3].set_value(request.view());
            return;
        }
        output.erase(2);
    });
    const auto decision_source = node([&](In<TS<i64>> cycle,
                                          Out<TSD<i64, TS<AuthDecision<i64>>>> output) {
        if (cycle.value() == 1) {
            output[2].set_value(decision_2.view());
            output[3].set_value(decision_3.view());
            return;
        }
        output[1].set_value(decision_1.view());
        output.erase(3);
    });

    struct IdentityChanges {
        std::vector<std::pair<i64, i64>> added;
        std::vector<i64> removed;
        std::vector<i64> request_keys;
        std::vector<i64> identity_keys;
    };
    auto changes = std::vector<IdentityChanges>{};
    const auto observe = node([&](In<TSD<i64, TS<Bundle<HttpServerRequest>>>, optional> accepted_requests,
                                  In<TSD<i64, TS<i64>>, optional> identities) {
        auto change = IdentityChanges{};
        accepted_requests.for_each_key([&](const Scalar<i64> &key) { change.request_keys.push_back(key.value()); });
        identities.for_each_key([&](const Scalar<i64> &key) { change.identity_keys.push_back(key.value()); });
        std::ranges::sort(change.request_keys);
        std::ranges::sort(change.identity_keys);
        identities.for_each_added([&](const Scalar<i64> &key) {
            change.added.emplace_back(key.value(), identities[key].value());
        });
        identities.for_each_removed([&](const Scalar<i64> &key) { change.removed.push_back(key.value()); });
        if (!change.added.empty() || !change.removed.empty()) changes.push_back(std::move(change));
    });
    const auto test_graph = [&](wp<TS<i64>> cycle) {
        const auto requests = request_source(cycle);
        const auto decisions = decision_source(cycle);
        const auto projection = bg::web::details::http_auth_projection(requests, decisions);
        observe(operators::getattr(projection, util::symbol{"requests"}),
                operators::getattr(projection, util::symbol{"identities"}));
        return cycle;
    };

    run_graph(test_graph, {1, 2});

    REQUIRE(changes.size() == 2);
    CHECK(changes[0].added == std::vector<std::pair<i64, i64>>{{2, 20}, {3, 30}});
    CHECK(changes[0].removed.empty());
    CHECK(changes[0].request_keys == changes[0].identity_keys);
    CHECK(changes[0].request_keys == std::vector<i64>{2, 3});
    CHECK(changes[1].added == std::vector<std::pair<i64, i64>>{{1, 10}});
    CHECK(changes[1].removed == std::vector<i64>{2, 3});
    CHECK(changes[1].request_keys == changes[1].identity_keys);
    CHECK(changes[1].request_keys == std::vector<i64>{1});
}

TEST_CASE("HTTP authentication rejects malformed decisions",
          "[feature:http][feature:service][feature:authentication]") {
    using Header = Tuple<string, string>;
    auto memory = allocator{};
    const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
    const auto body = ScalarValue<bytes>{std::span<const std::byte>{}};
    const auto peer = bundle<HttpPeer>(memory, "address"_n = std::string{"127.0.0.1"}, "port"_n = i64{1});
    const auto request = bundle<HttpServerRequest>(memory, "method"_n = std::string{"GET"},
                                                   "target"_n = std::string{"/auth"},
                                                   "path"_n = std::string{"/auth"}, "query"_n = headers.view(),
                                                   "headers"_n = headers.view(), "body"_n = body.view(),
                                                   "peer"_n = peer.view());
    const auto identity = ScalarValue<i64>{10};
    const auto response = make_http_response(401);
    const auto malformed = bundle<bg::web::details::AuthDecisionSchema<i64>::template schema>(
        memory, "identity"_n = identity.view(), "rejection"_n = response.view());
    const auto request_source = node([&](In<TS<i64>> cycle,
                                         Out<TSD<i64, TS<Bundle<HttpServerRequest>>>> requests) {
        if (cycle.value() != 1) return;
        requests[1].set_value(request.view());
    });
    const auto decision_source = node([&](In<TS<i64>> cycle,
                                          Out<TSD<i64, TS<AuthDecision<i64>>>> decisions) {
        if (cycle.value() != 1) return;
        decisions[1].set_value(malformed.view());
    });
    const auto test_graph = [&](wp<TS<i64>> cycle) {
        const auto requests = request_source(cycle);
        const auto decisions = decision_source(cycle);
        (void)bg::web::details::http_auth_projection(requests, decisions);
        return cycle;
    };

    CHECK_THROWS_AS(run_graph(test_graph, {1}), std::logic_error);
}

TEST_CASE("HTTP authentication requires a policy implementation",
          "[feature:http][feature:service][feature:authentication]") {
    const auto route = http_handler("/auth/missing", node<AuthenticatedHandler>(), auth_policy);
    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue);

    CHECK_THROWS(run_graph(
        [&] {
            (void)route();
            (void)http_server();
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{1}}));
}

TEST_CASE("HTTP authentication requires a matching identity input",
          "[feature:http][feature:authentication]") {
    const auto wrong_identity_handler = node([](In<TS<Bundle<HttpServerRequest>>>, In<TS<string>>,
                                                 Out<TS<Bundle<HttpResponse>>>) {});
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto route = http_handler("/auth/wrong-identity", wrong_identity_handler, auth_policy);

    CHECK_THROWS_AS(route(), WiringError);
}

TEST_CASE("HTTP server can start without routes", "[feature:http][feature:lifecycle]") {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = asio::ip::tcp;

    auto server = ServerTest{};
    auto status = 0U;
    server.run([] { (void)http_server(); }, [&](std::uint16_t port) {
        auto context = asio::io_context{};
        auto socket = tcp::socket{context};
        socket.connect({asio::ip::make_address("127.0.0.1"), port});
        auto request = http::request<http::empty_body>{http::verb::get, "/unregistered", 11};
        request.set(http::field::host, "localhost");
        http::write(socket, request);
        auto buffer = beast::flat_buffer{};
        auto reply = http::response<http::string_body>{};
        http::read(socket, buffer, reply);
        status = reply.result_int();
        server.stop.request_stop();
    });

    CHECK(status == 404);
}

TEST_CASE("HTTP server rejects duplicate routes", "[feature:http][feature:lifecycle]") {
    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue);
    const auto first = http_handler("/duplicate", echo_handler);
    const auto second = http_handler("/duplicate", echo_handler);

    CHECK_THROWS(run_graph(
        [&] {
            (void)first();
            (void)second();
            (void)http_server();
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = std::chrono::seconds{1}}));
}

TEST_CASE("HTTP route removal withdraws the route and permits a later registration",
          "[feature:http][feature:lifecycle][transition:removal][shape:nested]") {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = asio::ip::tcp;

    auto writer = Queue{};
    const auto activations = queue<TSS<i64>>(start = [&](Queue queue) { writer = queue; });
    const auto handler = http_handler("/dynamic", echo_handler);
    auto server = ServerTest{};
    auto statuses = std::vector<unsigned>{};

    server.run(
        [&] {
            (void)map([&](auto key) {
                (void)handler();
                return key;
            },
                      activations());
            (void)http_server();
        },
        [&](std::uint16_t port) {
            auto context = asio::io_context{};
            const auto request_status = [&] {
                auto socket = tcp::socket{context};
                socket.connect({asio::ip::make_address("127.0.0.1"), port});
                auto request = http::request<http::empty_body>{http::verb::get, "/dynamic", 11};
                request.set(http::field::host, "localhost");
                http::write(socket, request);
                auto buffer = beast::flat_buffer{};
                auto reply = http::response<http::string_body>{};
                http::read(socket, buffer, reply);
                return reply.result_int();
            };
            const auto wait_for_status = [&](unsigned expected) {
                auto status = 0U;
                for (auto attempt = 0; attempt < 100; ++attempt) {
                    try {
                        status = request_status();
                    } catch (const boost::system::system_error &error) {
                        if (error.code() != http::error::end_of_stream) throw;
                    }
                    if (status == expected) return status;
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                }
                return status;
            };

            if (!writer.write(::literal::TimeSeriesLiteral{1})) {
                throw std::runtime_error{"could not activate HTTP route"};
            }
            statuses.push_back(wait_for_status(200));
            if (!writer.write(::literal::TimeSeriesLiteral{literal::REMOVED(1)})) {
                throw std::runtime_error{"could not remove HTTP route"};
            }
            statuses.push_back(wait_for_status(404));
            if (!writer.write(::literal::TimeSeriesLiteral{1})) {
                throw std::runtime_error{"could not reactivate HTTP route"};
            }
            statuses.push_back(wait_for_status(200));
            server.stop.request_stop();
        });

    CHECK(statuses == std::vector<unsigned>{200, 404, 200});
}

} // namespace

TEST_CASE("HTTP server handles expectations and keep-alive requests",
          "[feature:http][feature:lifecycle][transition:stop]") {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = asio::ip::tcp;

    auto server = ServerTest{};
    auto statuses = std::vector<unsigned>{};
    auto bodies = std::vector<std::string>{};
    auto interim_status = 0U;
    auto missing_head_length = std::uint64_t{};

    server.run({Route{"/echo", echo_handler}}, [&](std::uint16_t port) {
        auto context = asio::io_context{};
        auto socket = tcp::socket{context};
        socket.connect({asio::ip::make_address("127.0.0.1"), port});

        auto request = http::request<http::string_body>{http::verb::post, "/echo", 11};
        request.set(http::field::host, "localhost");
        request.set(http::field::expect, "100-continue");
        request.body() = std::string{"\0binary\xff", 8};
        request.prepare_payload();
        auto serializer = http::request_serializer<http::string_body>{request};
        http::write_header(socket, serializer);

        auto buffer = beast::flat_buffer{};
        auto interim = http::response<http::empty_body>{};
        http::read(socket, buffer, interim);
        interim_status = interim.result_int();
        http::write(socket, serializer);

        auto response = http::response<http::string_body>{};
        http::read(socket, buffer, response);
        statuses.push_back(response.result_int());
        bodies.push_back(std::move(response.body()));

        auto missing_head = http::request<http::empty_body>{http::verb::head, "/missing", 11};
        missing_head.set(http::field::host, "localhost");
        http::write(socket, missing_head);
        auto missing_head_parser = http::response_parser<http::string_body>{};
        missing_head_parser.skip(true);
        http::read(socket, buffer, missing_head_parser);
        missing_head_length = missing_head_parser.content_length().value_or(0);
        auto missing_head_response = missing_head_parser.release();
        statuses.push_back(missing_head_response.result_int());

        auto missing = http::request<http::empty_body>{http::verb::get, "/missing", 11};
        missing.set(http::field::host, "localhost");
        http::write(socket, missing);
        auto missing_response = http::response<http::string_body>{};
        http::read(socket, buffer, missing_response);
        statuses.push_back(missing_response.result_int());

        auto second = http::request<http::string_body>{http::verb::post, "/echo", 11};
        second.set(http::field::host, "localhost");
        second.body() = "keep alive";
        second.prepare_payload();
        http::write(socket, second);
        auto second_response = http::response<http::string_body>{};
        http::read(socket, buffer, second_response);
        statuses.push_back(second_response.result_int());
        bodies.push_back(std::move(second_response.body()));

        auto unsupported = http::request<http::empty_body>{http::verb::get, "/echo", 11};
        unsupported.set(http::field::host, "localhost");
        unsupported.set(http::field::expect, "custom-expectation");
        http::write(socket, unsupported);
        auto unsupported_response = http::response<http::string_body>{};
        http::read(socket, buffer, unsupported_response);
        statuses.push_back(unsupported_response.result_int());
        server.stop.request_stop();
    });
    CHECK(interim_status == 100);
    CHECK(statuses == std::vector<unsigned>{200, 404, 404, 200, 417});
    CHECK(missing_head_length == 9);
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0] == std::string{"\0binary\xff", 8});
    CHECK(bodies[1] == "keep alive");
}

TEST_CASE("HTTP server route dictionaries isolate request additions and removals",
          "[feature:http][feature:lifecycle][transition:removal]") {
    using Header = Tuple<string, string>;
    auto route_a_events = std::vector<std::string>{};
    auto route_b_events = std::vector<std::string>{};
    const auto make_response = [](std::string_view text) {
        const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
        const auto body_bytes = std::span<const std::byte>{reinterpret_cast<const std::byte *>(text.data()),
                                                            text.size()};
        const auto body = ScalarValue<bytes>{body_bytes};
        return bundle<HttpResponse>("status"_n = i64{200}, "headers"_n = headers.view(), "body"_n = body.view());
    };
    const auto make_handler = [&](std::string route, std::vector<std::string> &events) {
        auto *route_events = &events;
        return node([&, route = std::move(route), route_events](In<TSD<i64, TS<Bundle<HttpServerRequest>>>> requests,
                                                                Out<TSD<i64, TS<Bundle<HttpResponse>>>> responses) {
            auto invocation = route + ":";
            auto changed = false;
            const auto record = [&](std::string_view kind, const auto &key) {
                if (changed) invocation.push_back(',');
                invocation.append(kind);
                invocation.append(std::to_string(key.value()));
                changed = true;
            };
            const auto respond = [&](const auto &key) {
                record("+", key);
                const auto response = make_response(route);
                responses[key].set_value(response.view());
            };
            requests.for_each_added(respond);
            requests.for_each_modified(respond);
            requests.for_each_removed([&](const auto &key) { record("-", key); });
            route_events->push_back(changed ? std::move(invocation) : route + ":empty");
        });
    };
    const auto route_a = make_handler("A", route_a_events);
    const auto route_b = make_handler("B", route_b_events);
    auto server = ServerTest{};
    auto statuses = std::vector<unsigned>{};
    auto replies = std::vector<std::string>{};

    server.run({Route{"/a", route_a}, Route{"/b", route_b}}, [&](std::uint16_t port) {
        namespace asio = boost::asio;
        namespace http = boost::beast::http;
        using tcp = asio::ip::tcp;

        auto context = asio::io_context{};
        auto socket = tcp::socket{context};
        socket.connect({asio::ip::make_address("127.0.0.1"), port});
        auto buffer = asio::streambuf{};
        for (const auto path : std::array<std::string_view, 4>{"/a", "/b", "/a", "/b"}) {
            auto request = http::request<http::empty_body>{http::verb::get, path, 11};
            request.set(http::field::host, "localhost");
            http::write(socket, request);
            const auto response = read_response(socket, buffer);
            statuses.push_back(response.status);
            replies.push_back(response.body);
        }
        server.stop.request_stop();
    });

    CHECK(statuses == std::vector<unsigned>{200, 200, 200, 200});
    CHECK(route_a_events == std::vector<std::string>{"A:+1", "A:-1", "A:+3", "A:-3"});
    REQUIRE((route_b_events.size() == 3 || route_b_events.size() == 4));
    CHECK(route_b_events[0] == "B:+2");
    CHECK(route_b_events[1] == "B:-2");
    CHECK(route_b_events[2] == "B:+4");
    if (route_b_events.size() == 4) CHECK(route_b_events[3] == "B:-4");
    CHECK(replies == std::vector<std::string>{"A", "B", "A", "B"});
}

TEST_CASE("HTTP server runtime state can be recreated from one compiled graph",
          "[feature:http][feature:lifecycle][transition:start][transition:stop]") {
    auto ports = std::vector<i64>{};
    auto *active_stop_source = static_cast<std::stop_source *>(nullptr);
    const auto capture_port = node([&](In<TS<i64>> port) {
        ports.push_back(port.value());
        active_stop_source->request_stop();
    });

    auto wiring = GraphWiring{};
    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue);
    auto handler = http_handler("/echo", echo_handler);
    {
        auto context = WiringContext{wiring};
        (void)handler();
        capture_port(http_server());
    }
    wiring.finalize(catalogue);
    auto compiled = CompiledGraph{wiring};

    for (auto run = 0; run < 2; ++run) {
        auto stop = std::stop_source{};
        active_stop_source = &stop;
        auto global_state = GlobalState{};
        auto runtime = RuntimeGraph{compiled, global_state};
        RealtimeEvaluationEngine{}.run(runtime, SystemWallClock::instance().now(), stop.get_token());
    }

    REQUIRE(ports.size() == 2);
    CHECK(ports[0] > 0);
    CHECK(ports[1] > 0);
}

TEST_CASE("HTTP server preserves duplicate fields and applies response framing rules",
          "[feature:http][feature:lifecycle]") {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = asio::ip::tcp;
    using Header = Tuple<string, string>;
    using Literal = literal::ScalarLiteral;

    struct RequestObservation {
        std::string method;
        std::vector<std::pair<std::string, std::string>> query;
        std::vector<std::pair<std::string, std::string>> headers;
    };

    auto observations = std::vector<RequestObservation>{};
    const auto inspect = node([&](In<TS<Bundle<HttpServerRequest>>> request,
                                  Out<TS<Bundle<HttpResponse>>> response) {
        const auto value = request.value();
        auto observation = RequestObservation{.method = std::string{scalar_cast<string>(value[0]).value()}};
        for (const auto [field, destination] :
             std::array{std::pair{value[3], &observation.query}, std::pair{value[4], &observation.headers}}) {
            const auto fields = scalar_cast<List<Header>>(field);
            for (auto index = std::size_t{}; index < fields.size(); ++index) {
                const auto item = fields[index];
                destination->emplace_back(std::string{scalar_cast<string>(item.template get<0>()).value()},
                                          std::string{scalar_cast<string>(item.template get<1>()).value()});
            }
        }
        observations.push_back(std::move(observation));

        auto status = i64{200};
        auto headers = std::vector<ScalarValue<Header>>{};
        auto header_views = std::vector<Scalar<Header>>{};
        headers.push_back(ScalarValue<Header>::from_literal(Literal{"X-Dup", "first"}));
        headers.push_back(ScalarValue<Header>::from_literal(Literal{"X-Middle", "middle"}));
        headers.push_back(ScalarValue<Header>::from_literal(Literal{"x-dup", "second"}));
        const auto &query = observations.back().query;
        for (const auto &[key, item] : query) {
            if (key == "case") {
                if (item == "204") status = 204;
                if (item == "205") status = 205;
                if (item == "304") status = 304;
                if (item == "bad-length") {
                    headers.push_back(ScalarValue<Header>::from_literal(Literal{"Content-Length", "99"}));
                }
            }
        }
        header_views.reserve(headers.size());
        for (const auto &header : headers) header_views.push_back(header.view());
        const auto response_headers = ScalarValue<List<Header>>{header_views};
        const auto body = std::array{std::byte{'r'}, std::byte{'e'}, std::byte{'p'}, std::byte{'l'}, std::byte{'y'}};
        const auto body_value = ScalarValue<bytes>{std::span<const std::byte>{body}};
        const auto reply = bundle<HttpResponse>("status"_n = status, "headers"_n = response_headers.view(),
                                                "body"_n = body_value.view());
        response.set_value(reply.view());
    });

    auto server = ServerTest{};
    auto statuses = std::vector<unsigned>{};
    auto bodies = std::vector<std::string>{};
    auto response_header_names = std::vector<std::string>{};
    auto content_lengths = std::vector<std::string>{};
    auto invalid_framing_closed = false;

    server.run({Route{"/test", inspect}}, [&](std::uint16_t port) {
        auto context = asio::io_context{};
        auto socket = tcp::socket{context};
        auto buffer = asio::streambuf{};
        socket.connect({asio::ip::make_address("127.0.0.1"), port});

        const auto record = [&](std::string request, bool head = false) {
            asio::write(socket, asio::buffer(request));
            const auto response = read_response(socket, buffer, head);
            statuses.push_back(response.status);
            bodies.push_back(response.body);
            const auto length = std::ranges::find_if(response.headers, [](const auto &header) {
                return boost::beast::iequals(header.first, "content-length");
            });
            content_lengths.emplace_back(length == response.headers.end() ? "" : length->second);
            if (statuses.size() == 1) {
                for (const auto &[name, value] : response.headers) {
                    (void)value;
                    if (boost::beast::iequals(name, "x-dup") || boost::beast::iequals(name, "x-middle")) {
                        response_header_names.push_back(name);
                    }
                }
            }
        };

        record("GET /test?dup=one&dup=two HTTP/1.1\r\nHost: localhost\r\nX-Dup: first\r\n"
               "X-Middle: middle\r\nx-dup: second\r\n\r\n");
        record("HEAD /test?case=head HTTP/1.1\r\nHost: localhost\r\n\r\n", true);
        record("GET /test?case=204 HTTP/1.1\r\nHost: localhost\r\n\r\n");
        record("GET /test?case=205 HTTP/1.1\r\nHost: localhost\r\n\r\n");
        record("GET /test?case=304 HTTP/1.1\r\nHost: localhost\r\n\r\n");
        record("GET /test?case=bad-length HTTP/1.1\r\nHost: localhost\r\n\r\n");
        socket.close();

        auto invalid_socket = tcp::socket{context};
        invalid_socket.connect({asio::ip::make_address("127.0.0.1"), port});
        const auto malformed = std::string{
            "POST /test HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n"
            "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"};
        asio::write(invalid_socket, asio::buffer(malformed));
        auto byte = std::array<char, 1>{};
        beast::error_code error;
        (void)invalid_socket.read_some(asio::buffer(byte), error);
        invalid_framing_closed = static_cast<bool>(error);
        server.stop.request_stop();
    });

    REQUIRE(statuses == std::vector<unsigned>{200, 200, 204, 205, 304, 500});
    CHECK(bodies[0] == "reply");
    CHECK(bodies[1].empty());
    CHECK(bodies[2].empty());
    CHECK(bodies[3].empty());
    CHECK(bodies[4].empty());
    CHECK(bodies[5] == "Invalid Response Header");
    CHECK(response_header_names == std::vector<std::string>{"X-Dup", "X-Middle", "x-dup"});
    CHECK(content_lengths == std::vector<std::string>{"5", "5", "", "0", "5", "23"});
    REQUIRE(observations.size() == 6);
    CHECK(observations[0].query == std::vector<std::pair<std::string, std::string>>{
                                         {"dup", "one"}, {"dup", "two"}});
    auto duplicate_headers = std::vector<std::pair<std::string, std::string>>{};
    for (const auto &header : observations[0].headers) {
        if (boost::beast::iequals(header.first, "x-dup") || boost::beast::iequals(header.first, "x-middle")) {
            duplicate_headers.push_back(header);
        }
    }
    CHECK(duplicate_headers == std::vector<std::pair<std::string, std::string>>{
                                   {"X-Dup", "first"}, {"X-Middle", "middle"}, {"x-dup", "second"}});
    CHECK(observations[1].method == "HEAD");
    CHECK(invalid_framing_closed);
}

TEST_CASE("HTTP server parses origin-form paths and query parameters", "[feature:http]") {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    using Header = Tuple<string, string>;

    struct Observation {
        std::string path;
        std::vector<std::pair<std::string, std::string>> query;
    };
    auto observations = std::vector<Observation>{};
    const auto inspect = node([&](In<TS<Bundle<HttpServerRequest>>> request,
                                  Out<TS<Bundle<HttpResponse>>> response) {
        const auto value = request.value();
        auto observation = Observation{.path = std::string{scalar_cast<string>(value[2]).value()}};
        const auto query = scalar_cast<List<Header>>(value[3]);
        for (auto index = std::size_t{}; index < query.size(); ++index) {
            const auto item = query[index];
            observation.query.emplace_back(std::string{scalar_cast<string>(item.template get<0>()).value()},
                                           std::string{scalar_cast<string>(item.template get<1>()).value()});
        }
        observations.push_back(std::move(observation));
        const auto headers = ScalarValue<List<Header>>{std::span<const Scalar<Header>>{}};
        const auto body = ScalarValue<bytes>{std::span<const std::byte>{}};
        const auto reply = bundle<HttpResponse>("status"_n = i64{200}, "headers"_n = headers.view(),
                                                "body"_n = body.view());
        response.set_value(reply.view());
    });

    auto server = ServerTest{};
    auto valid_statuses = std::vector<unsigned>{};
    auto invalid_statuses = std::vector<unsigned>{};
    server.run({Route{"/path", inspect}, Route{"/a+b", inspect}}, [&](std::uint16_t port) {
        auto context = asio::io_context{};
        auto socket = tcp::socket{context};
        auto buffer = asio::streambuf{};
        socket.connect({asio::ip::make_address("127.0.0.1"), port});
        for (const auto target : {"/p%61th?space=a+b&empty=&flag&duplicate=x&duplicate=y%2Bz", "/a+b"}) {
            const auto request = std::string{"GET "} + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
            asio::write(socket, asio::buffer(request));
            valid_statuses.push_back(read_response(socket, buffer).status);
        }
        socket.close();

        const auto read_invalid = [&](std::string request) {
            auto invalid_socket = tcp::socket{context};
            auto invalid_buffer = asio::streambuf{};
            invalid_socket.connect({asio::ip::make_address("127.0.0.1"), port});
            asio::write(invalid_socket, asio::buffer(request));
            invalid_statuses.push_back(read_response(invalid_socket, invalid_buffer).status);
            invalid_socket.close();
        };
        for (const auto target : {"/path?bad=%GG", "/path#fragment", "http://localhost/path"}) {
            read_invalid(std::string{"GET "} + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
        }
        read_invalid("GET /path HTTP/1.1\r\n\r\n");
        read_invalid("GET /path HTTP/1.1\r\nHost: localhost\r\nHost: example.com\r\n\r\n");
        read_invalid("GET /path HTTP/1.1\r\nHost:\r\n\r\n");
        read_invalid("GET /path HTTP/1.1\r\nHost: /invalid\r\n\r\n");
        read_invalid("GET /path HTTP/1.1\r\nHost: user@localhost\r\n\r\n");
        server.stop.request_stop();
    });

    CHECK(valid_statuses == std::vector<unsigned>{200, 200});
    CHECK(invalid_statuses == std::vector<unsigned>{400, 400, 400, 400, 400, 400, 400, 400});
    REQUIRE(observations.size() == 2);
    CHECK(observations[0].path == "/path");
    CHECK(observations[0].query == std::vector<std::pair<std::string, std::string>>{
                                      {"space", "a b"}, {"empty", ""}, {"flag", ""}, {"duplicate", "x"},
                                      {"duplicate", "y+z"}});
    CHECK(observations[1].path == "/a+b");
}

TEST_CASE("HTTP server rejects incompatible route signatures", "[feature:http]") {
    const auto no_arguments = node([] {});
    const auto no_output = node([](In<TS<Bundle<HttpServerRequest>>>) {});
    const auto wrong_output = node([](In<TS<Bundle<HttpServerRequest>>>, Out<TS<i64>>) {});

    CHECK_THROWS_AS(http_handler("/zero", no_arguments), std::invalid_argument);
    CHECK_THROWS_AS(http_handler("/no-output", no_output), std::invalid_argument);

    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    auto handler = http_handler("/wrong-output", wrong_output);
    CHECK_THROWS_AS(handler(), WiringError);
}

TEST_CASE("HTTP server removes disconnected requests before replying to later requests",
          "[feature:http][feature:lifecycle][transition:removal]") {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = asio::ip::tcp;
    using Header = Tuple<string, string>;

    auto mutex = std::mutex{};
    auto changed = std::condition_variable{};
    auto first_id = i64{};
    auto first_seen = false;
    auto first_removed = false;
    auto second_response = std::string{};
    const auto delayed = node([&](In<TSD<i64, TS<Bundle<HttpServerRequest>>>> requests,
                                  Out<TSD<i64, TS<Bundle<HttpResponse>>>> responses) {
        requests.for_each_added([&](const auto &key) {
            {
                auto lock = std::lock_guard{mutex};
                if (!first_seen) {
                    first_id = key.value();
                    first_seen = true;
                    changed.notify_all();
                    return;
                }
            }

            const auto reply = [&](std::string_view text) {
                const auto status = ScalarValue<i64>{200};
                const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
                const auto body_bytes = std::span<const std::byte>{
                    reinterpret_cast<const std::byte *>(text.data()), text.size()};
                const auto body = ScalarValue<bytes>{body_bytes};
                const auto value = bundle<HttpResponse>("status"_n = status.view(), "headers"_n = headers.view(),
                                                        "body"_n = body.view());
                return ScalarValue<Bundle<HttpResponse>>{value.view()};
            };

            const auto stale = reply("stale");
            responses[first_id].set_value(stale.view());
            const auto current = reply("second");
            responses[key].set_value(current.view());
            {
                auto lock = std::lock_guard{mutex};
                second_response = "second";
            }
            changed.notify_all();
        });
        requests.for_each_removed([&](const auto &key) {
            auto lock = std::lock_guard{mutex};
            if (key.value() == first_id) {
                first_removed = true;
                changed.notify_all();
            }
        });
    });

    auto server = ServerTest{};
    auto client_reply = std::string{};
    auto removal_observed = false;

    server.run({Route{"/pending", delayed}}, [&](std::uint16_t port) {
        auto context = asio::io_context{};
        auto first_socket = tcp::socket{context};
        first_socket.connect({asio::ip::make_address("127.0.0.1"), port});
        auto first = http::request<http::empty_body>{http::verb::get, "/pending", 11};
        first.set(http::field::host, "localhost");
        http::write(first_socket, first);

        {
            auto lock = std::unique_lock{mutex};
            if (!changed.wait_for(lock, std::chrono::seconds{3}, [&] { return first_seen; })) {
                throw std::runtime_error{"first HTTP request was not delivered"};
            }
        }
        beast::error_code ignored;
        first_socket.shutdown(tcp::socket::shutdown_both, ignored);
        first_socket.close(ignored);

        {
            auto lock = std::unique_lock{mutex};
            if (!changed.wait_for(lock, std::chrono::seconds{3}, [&] { return first_removed; })) {
                throw std::runtime_error{"disconnected HTTP request was not removed"};
            }
            removal_observed = first_removed;
        }

        auto second_socket = tcp::socket{context};
        second_socket.connect({asio::ip::make_address("127.0.0.1"), port});
        auto second = http::request<http::empty_body>{http::verb::get, "/pending", 11};
        second.set(http::field::host, "localhost");
        http::write(second_socket, second);
        auto buffer = beast::flat_buffer{};
        auto response = http::response<http::string_body>{};
        http::read(second_socket, buffer, response);
        client_reply = std::move(response.body());
        server.stop.request_stop();
    });
    CHECK(removal_observed);
    CHECK(second_response == "second");
    CHECK(client_reply == "second");
}

TEST_CASE("HTTP server closes a connection with a pending request when the graph stops",
          "[feature:http][feature:lifecycle][transition:stop]") {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    using tcp = asio::ip::tcp;

    auto server = ServerTest{};
    auto request_seen = false;
    const auto hold_open = node([&](In<TS<Bundle<HttpServerRequest>>> request,
                                    Out<TS<Bundle<HttpResponse>>>) {
        if (request.valid() && !request_seen) {
            request_seen = true;
            server.stop.request_stop();
        }
    });
    auto pending_connection_closed = false;

    server.run({Route{"/pending", hold_open}}, [&](std::uint16_t port) {
        auto context = asio::io_context{};
        auto socket = tcp::socket{context};
        socket.connect({asio::ip::make_address("127.0.0.1"), port});
        const auto request = std::string{
            "GET /pending HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"};
        asio::write(socket, asio::buffer(request));
        auto byte = std::array<char, 1>{};
        beast::error_code error;
        (void)socket.read_some(asio::buffer(byte), error);
        pending_connection_closed = static_cast<bool>(error);
    });
    CHECK(request_seen);
    CHECK(pending_connection_closed);
}
