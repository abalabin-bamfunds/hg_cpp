#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/library/operators.hpp>
#include <hg/literal/scalar_literal.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/queue_node.hpp>
#include <hg/run_graph.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/scalar/list/scalar.hpp>
#include <hg/scalar/tuple/scalar.hpp>
#include <hg/service_catalogue.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/util/scope_exit.hpp>
#include <hg/web/http_client.hpp>
#include <hg/web/http_server.hpp>
#include <hg/web/websocket_client.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace hg;
using namespace hg::literals;
using namespace hg::web;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using Header = Tuple<string, string>;

constexpr std::size_t ClientCount = 16;
constexpr auto GraphTimeout = std::chrono::minutes{10};

struct Options {
    std::size_t messages_per_sample{4096};
    std::size_t samples{5};
    std::size_t warmup_messages{128};
    std::optional<std::filesystem::path> output;
    std::vector<std::string> protocols;
    std::vector<std::size_t> windows;
    std::vector<std::size_t> payload_sizes;
};

struct Sample {
    std::vector<std::int64_t> latency_ns;
    std::int64_t elapsed_ns{};
    std::size_t peer_ports{};
    std::size_t batches{};
    std::size_t max_batch{};
};

struct Result {
    std::string protocol;
    std::string mode;
    std::size_t payload_bytes{};
    std::size_t window{};
    std::vector<Sample> samples;
};

[[nodiscard]] std::uint64_t payload_sequence(std::span<const std::byte> payload) {
    if (payload.size() < 12) throw std::runtime_error{"benchmark payload is shorter than its identity header"};
    auto sequence = std::uint64_t{};
    std::memcpy(&sequence, payload.data(), sizeof(sequence));
    return sequence;
}

[[nodiscard]] std::uint32_t payload_client_id(std::span<const std::byte> payload) {
    if (payload.size() < 12) throw std::runtime_error{"benchmark payload is shorter than its identity header"};
    auto client_id = std::uint32_t{};
    std::memcpy(&client_id, payload.data() + sizeof(std::uint64_t), sizeof(client_id));
    return client_id;
}

[[nodiscard]] std::vector<std::byte> make_payload(std::size_t size, std::uint64_t sequence,
                                                  std::uint32_t client_id) {
    if (size < 12) throw std::invalid_argument{"payload size must hold a sequence and client ID"};
    auto result = std::vector<std::byte>(size);
    for (auto index = std::size_t{12}; index < size; ++index) {
        result[index] = static_cast<std::byte>((sequence + client_id * 31 + index * 17) & 0xff);
    }
    std::memcpy(result.data(), &sequence, sizeof(sequence));
    std::memcpy(result.data() + sizeof(sequence), &client_id, sizeof(client_id));
    return result;
}

[[nodiscard]] bool payload_matches(std::span<const std::byte> actual, std::size_t expected_size,
                                   std::uint64_t sequence, std::uint32_t client_id) {
    if (actual.size() != expected_size) return false;
    const auto expected = make_payload(expected_size, sequence, client_id);
    return std::ranges::equal(actual, expected);
}

[[nodiscard]] ScalarValue<Bundle<HttpClientRequest>> make_request(std::string url,
                                                                   std::span<const std::byte> body) {
    const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
    const auto body_value = ScalarValue<bytes>{body};
    auto memory = allocator{};
    return bundle<HttpClientRequest>(memory, "method"_n = "POST", "url"_n = url,
                                     "headers"_n = headers.view(), "body"_n = body_value.view());
}

template <typename Body> [[nodiscard]] ScalarValue<Bundle<HttpResponse>> make_http_response(Body body) {
    const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
    auto memory = allocator{};
    return bundle<HttpResponse>(memory, "status"_n = i64{200}, "headers"_n = headers.view(), "body"_n = body);
}

[[nodiscard]] std::string test_data_file(std::string_view name) {
    return (std::filesystem::path{HG_WEB_BENCHMARK_TEST_DATA} / name).string();
}

[[nodiscard]] double percentile(std::vector<std::int64_t> values, double fraction) {
    if (values.empty()) return 0;
    std::ranges::sort(values);
    const auto index = std::min(values.size() - 1,
                                static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1);
    return static_cast<double>(values[index]);
}

[[nodiscard]] std::string compiler_name() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#else
    return "unknown";
#endif
}

void write_sample(std::ostream &output, const Result &result, std::size_t index) {
    const auto &sample = result.samples[index];
    const auto elapsed_seconds = static_cast<double>(sample.elapsed_ns) / 1'000'000'000.0;
    output << "{\"kind\":\"sample\",\"protocol\":\"" << result.protocol << "\",\"mode\":\""
           << result.mode << "\",\"payload_bytes\":" << result.payload_bytes << ",\"window\":" << result.window
           << ",\"sample\":" << index << ",\"messages\":" << sample.latency_ns.size()
           << ",\"elapsed_ns\":" << sample.elapsed_ns << ",\"throughput_per_second\":" << std::fixed
           << std::setprecision(2)
           << (elapsed_seconds > 0 ? static_cast<double>(sample.latency_ns.size()) / elapsed_seconds : 0)
           << ",\"payload_mib_per_second\":"
           << (elapsed_seconds > 0 ? static_cast<double>(sample.latency_ns.size()) * result.payload_bytes /
                                         (elapsed_seconds * 1024.0 * 1024.0)
                                   : 0)
           << ",\"p50_ns\":" << percentile(sample.latency_ns, 0.50) << ",\"p95_ns\":"
           << percentile(sample.latency_ns, 0.95) << ",\"p99_ns\":" << percentile(sample.latency_ns, 0.99)
           << ",\"peer_ports\":" << sample.peer_ports << ",\"ingress_batches\":" << sample.batches
           << ",\"max_ingress_batch\":" << sample.max_batch << ",\"latency_ns\":[";
    for (auto item = std::size_t{}; item < sample.latency_ns.size(); ++item) {
        if (item != 0) output << ',';
        output << sample.latency_ns[item];
    }
    output << "]}\n";
}

void write_metadata(std::ostream &output, const Options &options) {
    output << "{\"kind\":\"metadata\",\"benchmark\":\"web_extension\",\"build\":\"Release\",\"compiler\":\""
           << compiler_name() << "\",\"hardware_threads\":" << std::thread::hardware_concurrency()
           << ",\"messages_per_sample\":" << options.messages_per_sample << ",\"samples\":" << options.samples
           << ",\"warmup_messages\":" << options.warmup_messages << ",\"protocols\":[";
    for (auto index = std::size_t{}; index < options.protocols.size(); ++index) {
        if (index != 0) output << ',';
        output << '"' << options.protocols[index] << '"';
    }
    output << "],\"windows\":[";
    for (auto index = std::size_t{}; index < options.windows.size(); ++index) {
        if (index != 0) output << ',';
        output << options.windows[index];
    }
    output << "],\"payload_sizes\":[";
    for (auto index = std::size_t{}; index < options.payload_sizes.size(); ++index) {
        if (index != 0) output << ',';
        output << options.payload_sizes[index];
    }
    output << "]}\n";
}

[[nodiscard]] Options parse_options(int argc, char **argv) {
    auto options = Options{};
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--quick") {
            options.messages_per_sample = 64;
            options.samples = 1;
            continue;
        }
        if (index + 1 == argc) throw std::invalid_argument{"missing value for " + std::string{argument}};
        const auto value = std::string_view{argv[++index]};
        if (argument == "--messages") {
            options.messages_per_sample = static_cast<std::size_t>(std::stoull(std::string{value}));
        } else if (argument == "--samples") {
            options.samples = static_cast<std::size_t>(std::stoull(std::string{value}));
        } else if (argument == "--output") {
            options.output = std::filesystem::path{value};
        } else if (argument == "--protocol") {
            const auto protocol = std::string{value};
            if (protocol != "http" && protocol != "https" && protocol != "ws" && protocol != "wss") {
                throw std::invalid_argument{"protocol must be http, https, ws, or wss"};
            }
            if (std::ranges::find(options.protocols, protocol) != options.protocols.end()) {
                throw std::invalid_argument{"protocol was selected more than once"};
            }
            options.protocols.push_back(protocol);
        } else if (argument == "--window") {
            const auto window = static_cast<std::size_t>(std::stoull(std::string{value}));
            if (window == 0) throw std::invalid_argument{"window must be positive"};
            if (std::ranges::find(options.windows, window) != options.windows.end()) {
                throw std::invalid_argument{"window was selected more than once"};
            }
            options.windows.push_back(window);
        } else if (argument == "--payload") {
            const auto payload_size = static_cast<std::size_t>(std::stoull(std::string{value}));
            if (payload_size < 12) throw std::invalid_argument{"payload size must be at least 12 bytes"};
            if (std::ranges::find(options.payload_sizes, payload_size) != options.payload_sizes.end()) {
                throw std::invalid_argument{"payload size was selected more than once"};
            }
            options.payload_sizes.push_back(payload_size);
        } else {
            throw std::invalid_argument{"unknown option " + std::string{argument}};
        }
    }
    if (options.messages_per_sample == 0 || options.samples == 0) {
        throw std::invalid_argument{"message and sample counts must be positive"};
    }
    return options;
}

struct HttpRun {
    HttpRun(std::size_t payload_size, std::size_t active_window, std::size_t sample_count,
            std::size_t sample_messages, std::size_t warmup_count)
        : payload_bytes{payload_size}, window{active_window}, sample_count{sample_count},
          sample_messages{sample_messages}, warmup_count{warmup_count}, samples(sample_count),
          timestamps(warmup_count + sample_count * sample_messages + 1),
          expected_client(timestamps.size()), sequence_sample(timestamps.size(), -1),
          seen(timestamps.size()), active(active_window), writers(active_window),
          peer_ports(sample_count) {}

    const std::size_t payload_bytes;
    const std::size_t window;
    const std::size_t sample_count;
    const std::size_t sample_messages;
    const std::size_t warmup_count;
    std::vector<Sample> samples;
    std::vector<Clock::time_point> timestamps;
    std::vector<std::uint32_t> expected_client;
    std::vector<std::int32_t> sequence_sample;
    std::vector<bool> seen;
    std::vector<bool> active;
    std::vector<Queue> writers;
    std::vector<std::set<std::uint16_t>> peer_ports;
    std::uint64_t next_sequence{1};
    std::size_t warmup_sent{};
    std::size_t warmup_received{};
    std::size_t sample_index{};
    std::size_t sample_sent{};
    std::size_t sample_received{};
    std::optional<Clock::time_point> sample_started;
    std::uint16_t port{};
    bool started{};
    bool done{};
    std::string scheme{"http"};
    std::stop_source stop;

    void start(std::uint16_t bound_port) {
        if (started) return;
        started = true;
        port = bound_port;
        for (auto client = std::size_t{}; client < window; ++client) {
            if (!writers[client].is_open()) throw std::runtime_error{"HTTP request queue did not start"};
            issue(client);
        }
    }

    void issue(std::size_t client) {
        if (done || active[client] || !writers[client].is_open()) return;
        auto sequence = std::uint64_t{};
        if (warmup_sent < warmup_count) {
            sequence = next_sequence++;
            ++warmup_sent;
        } else if (warmup_received == warmup_count && warmup_sent == warmup_count &&
                   sample_index < sample_count && sample_sent < sample_messages) {
            sequence = next_sequence++;
            if (sample_sent == 0) sample_started = Clock::now();
            ++sample_sent;
            sequence_sample[sequence] = static_cast<std::int32_t>(sample_index);
        } else {
            return;
        }

        const auto body = make_payload(payload_bytes, sequence, static_cast<std::uint32_t>(client));
        timestamps[sequence] = Clock::now();
        expected_client[sequence] = static_cast<std::uint32_t>(client);
        const auto url = scheme + "://" + (scheme == "https" ? "localhost" : "127.0.0.1") + ":" +
                         std::to_string(port) + "/echo";
        const auto request = make_request(url, body);
        if (!writers[client].write(literal::TimeSeriesLiteral(request.view()))) {
            throw std::runtime_error{"HTTP request queue rejected a benchmark request"};
        }
        active[client] = true;
    }

    void observe_server_request(Scalar<Bundle<HttpServerRequest>> request) {
        const auto body = scalar_cast<bytes>(request[5]).value();
        const auto sequence = payload_sequence(body);
        const auto client = payload_client_id(body);
        if (client >= window || sequence >= sequence_sample.size()) {
            throw std::runtime_error{"server observed an invalid request identity"};
        }
        const auto peer = scalar_cast<Bundle<HttpPeer>>(request[6]);
        const auto sample = sequence_sample[sequence];
        if (sample >= 0) {
            peer_ports[static_cast<std::size_t>(sample)].insert(
                static_cast<std::uint16_t>(scalar_cast<i64>(peer[1]).value()));
        }
    }

    void reply(std::size_t client, Scalar<Bundle<HttpResponse>> response) {
        const auto received = Clock::now();
        const auto status = scalar_cast<i64>(response[0]).value();
        const auto body = scalar_cast<bytes>(response[2]).value();
        const auto sequence = payload_sequence(body);
        if (status != 200) throw std::runtime_error{"HTTP benchmark received status " + std::to_string(status)};
        if (sequence >= seen.size()) throw std::runtime_error{"HTTP benchmark received an unknown sequence"};
        if (seen[sequence]) throw std::runtime_error{"HTTP benchmark received a duplicate sequence"};
        if (expected_client[sequence] != client) {
            throw std::runtime_error{"HTTP benchmark response arrived on the wrong client"};
        }
        if (!payload_matches(body, payload_bytes, sequence, static_cast<std::uint32_t>(client))) {
            throw std::runtime_error{"HTTP benchmark response payload did not match its request"};
        }
        seen[sequence] = true;
        active[client] = false;
        const auto index = sequence_sample[sequence];
        auto phase_finished = false;
        if (index < 0) {
            ++warmup_received;
            phase_finished = warmup_received == warmup_count && warmup_sent == warmup_count;
        } else {
            auto &sample = samples[static_cast<std::size_t>(index)];
            sample.latency_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            received - timestamps[sequence])
                                            .count());
            ++sample_received;
            if (sample_received == sample_messages) {
                sample.elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - *sample_started)
                                        .count();
                sample.peer_ports = peer_ports[sample_index].size();
                ++sample_index;
                sample_sent = 0;
                sample_received = 0;
                sample_started.reset();
                phase_finished = true;
            }
        }

        if (phase_finished) {
            for (auto slot = std::size_t{}; slot < window; ++slot) issue(slot);
        } else {
            issue(client);
        }
        if (sample_index == sample_count && sample_received == 0) {
            done = true;
            stop.request_stop();
        }
    }

};

[[nodiscard]] Result run_http(const Options &options, std::string protocol, std::size_t payload_bytes,
                              std::size_t window) {
    auto state = HttpRun{payload_bytes, window, options.samples, options.messages_per_sample,
                         options.warmup_messages};
    state.scheme = protocol;
    auto catalogue = ServiceCatalogue{};
    auto client_options = HttpClientOptions{};
    if (protocol == "https") client_options.ca_file = test_data_file("test_server_cert.pem");
    catalogue.register_impl(http_client, http_client_impl(std::move(client_options)), {});
    auto server_options = ServerOptions{};
    if (protocol == "https") {
        server_options.tls = ServerTlsOptions{test_data_file("test_server_cert.pem"),
                                              test_data_file("test_server_key.pem")};
    }
    register_http_server(catalogue, std::move(server_options));

    const auto handler = node([&state](In<TS<Bundle<HttpServerRequest>>> request,
                                      Out<TS<Bundle<HttpResponse>>> response) {
        state.observe_server_request(request.value());
        const auto reply = make_http_response(request.value()[5]);
        response.set_value(reply.view());
    });
    const auto route = http_handler("/echo", handler);

    run_graph(
        [&] {
            (void)route();
            for (auto client = std::size_t{}; client < window; ++client) {
                const auto requests = queue<TS<Bundle<HttpClientRequest>>>(
                    start = [&state, client](Queue writer) { state.writers[client] = std::move(writer); })().value();
                const auto response = http_client(requests);
                (void)node([&state, client](In<TS<Bundle<HttpResponse>>> reply) {
                    if (reply.valid() && reply.modified()) state.reply(client, reply.value());
                })(response);
            }
            (void)node([&state](In<TS<i64>> port) {
                if (port.valid() && port.modified()) state.start(static_cast<std::uint16_t>(*port));
            })(http_server());
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = GraphTimeout,
                        .stop_token = state.stop.get_token()});

    if (!state.done) throw std::runtime_error{"HTTP benchmark graph stopped before all responses arrived"};
    return Result{std::move(protocol), "standalone_anonymous_bg_client_bg_server", payload_bytes, window,
                  std::move(state.samples)};
}

struct WebSocketRun {
    WebSocketRun(std::size_t payload_size, std::size_t active_window, std::size_t sample_count,
                 std::size_t sample_messages, std::size_t warmup_count)
        : payload_bytes{payload_size}, window{active_window}, sample_count{sample_count},
          sample_messages{sample_messages}, warmup_count{warmup_count},
          samples(sample_count), timestamps(warmup_count + sample_count * sample_messages + 1),
          seen(timestamps.size()), sequence_sample(timestamps.size(), -1) {}

    const std::size_t payload_bytes;
    const std::size_t window;
    const std::size_t sample_count;
    const std::size_t sample_messages;
    const std::size_t warmup_count;
    std::vector<Sample> samples;
    std::vector<Clock::time_point> timestamps;
    std::vector<bool> seen;
    std::vector<std::int32_t> sequence_sample;
    Queue writer;
    std::uint64_t next_sequence{1};
    std::uint64_t expected_ingress{1};
    std::uint64_t expected_reply{1};
    std::size_t in_flight{};
    std::size_t warmup_sent{};
    std::size_t warmup_received{};
    std::size_t sample_index{};
    std::size_t sample_sent{};
    std::size_t sample_received{};
    std::optional<Clock::time_point> sample_started;
    bool connected{};
    bool done{};
    std::stop_source stop;

    void record_ingress(Scalar<List<bytes>> messages) {
        if (sample_index < sample_count && sample_started) {
            auto &sample = samples[sample_index];
            ++sample.batches;
            sample.max_batch = std::max(sample.max_batch, messages.size());
        }
        for (auto index = std::size_t{}; index < messages.size(); ++index) {
            const auto body = messages[index].value();
            const auto sequence = payload_sequence(body);
            if (sequence != expected_ingress++ || !payload_matches(body, payload_bytes, sequence, 0)) {
                throw std::runtime_error{"WebSocket ingress validation failed"};
            }
        }
    }

    void send_available() {
        while (!done && in_flight < window) {
            auto sequence = std::uint64_t{};
            if (warmup_sent < warmup_count) {
                sequence = next_sequence++;
                ++warmup_sent;
            } else if (warmup_sent == warmup_count && warmup_received == warmup_count &&
                       sample_index < sample_count && sample_sent < sample_messages) {
                sequence = next_sequence++;
                if (sample_sent == 0) sample_started = Clock::now();
                ++sample_sent;
                sequence_sample[sequence] = static_cast<std::int32_t>(sample_index);
            } else {
                return;
            }

            const auto body = make_payload(payload_bytes, sequence, 0);
            timestamps[sequence] = Clock::now();
            if (!writer.write(literal::TimeSeriesLiteral{{"message", literal::ScalarLiteral(
                                                               std::span<const std::byte>{body})}})) {
                throw std::runtime_error{"WebSocket message queue rejected a benchmark message"};
            }
            ++in_flight;
        }
    }

    void connected_with(i64 status) {
        if (status != 101) throw std::runtime_error{"WebSocket benchmark handshake failed"};
        connected = true;
        send_available();
    }

    void receive(Scalar<bytes> message) {
        const auto received = Clock::now();
        const auto body = message.value();
        const auto sequence = payload_sequence(body);
        if (sequence != expected_reply || sequence >= seen.size() || seen[sequence] ||
            !payload_matches(body, payload_bytes, sequence, 0) || in_flight == 0) {
            throw std::runtime_error{"WebSocket benchmark response validation failed"};
        }
        ++expected_reply;
        seen[sequence] = true;
        --in_flight;
        const auto index = sequence_sample[sequence];
        if (index < 0) {
            ++warmup_received;
        } else {
            auto &sample = samples[static_cast<std::size_t>(index)];
            sample.latency_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            received - timestamps[sequence])
                                            .count());
            ++sample_received;
            if (sample_received == sample_messages) {
                sample.elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - *sample_started)
                                        .count();
                ++sample_index;
                sample_sent = 0;
                sample_received = 0;
                sample_started.reset();
            }
        }

        send_available();
        if (sample_index == sample_count && in_flight == 0) {
            done = true;
            stop.request_stop();
        }
    }
};

[[nodiscard]] ScalarValue<Bundle<WebSocketConnectRequest>> make_connect_request(std::uint16_t port, bool secure) {
    const auto headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
    const auto url = std::string{secure ? "wss://localhost:" : "ws://127.0.0.1:"} + std::to_string(port) + "/socket";
    auto memory = allocator{};
    return bundle<WebSocketConnectRequest>(memory, "url"_n = url, "headers"_n = headers.view());
}

[[nodiscard]] Result run_websocket(const Options &options, std::size_t payload_bytes, std::size_t window, bool secure) {
    auto state = WebSocketRun{payload_bytes, window, options.samples, options.messages_per_sample,
                              options.warmup_messages};
    auto catalogue = ServiceCatalogue{};
    auto client_options = WebSocketClientOptions{};
    auto server_options = ServerOptions{};
    if (secure) {
        client_options.ca_file = test_data_file("test_server_cert.pem");
        server_options.tls = ServerTlsOptions{test_data_file("test_server_cert.pem"),
                                              test_data_file("test_server_key.pem")};
    }
    catalogue.register_impl(websocket_client, websocket_client_impl(std::move(client_options)), {});
    register_http_server(catalogue, std::move(server_options));

    const auto handler = graph([&state](wp<TSB<WebSocketServerRequest<bytes>::schema>> request) {
        const auto incoming = request->messages;
        (void)node([&state](In<TS<List<bytes>>> messages) {
            if (!messages.valid() || !messages.modified()) return;
            state.record_ingress(messages.value_view());
        })(incoming);

        const auto response_headers = ScalarValue<List<Header>>{std::vector<Scalar<Header>>{}};
        const auto response_body = ScalarValue<bytes>{std::span<const std::byte>{}};
        auto memory = allocator{};
        const auto accepted = bundle<HttpResponse>(memory, "status"_n = i64{101}, "headers"_n = response_headers.view(),
                                                    "body"_n = response_body.view());
        return bundle<WebSocketResponse<bytes>::schema>(
            "connect_response"_n = operators::const_(accepted.view()), "message"_n = operators::emit(incoming),
            "close"_n = operators::nothing[type<TS<Bundle<WebSocketClose>>>]());
    });
    const auto route = websocket_handler("/socket", handler);

    run_graph(
        [&] {
            (void)route();
            const auto requests = queue<TSB<BinaryWebSocketClientRequest>>(
                start = [&state](Queue writer) { state.writer = std::move(writer); })().value();
            const auto response = websocket_client(requests);
            (void)node([&state](In<TSB<BinaryWebSocketClientReply>> reply) {
                if (reply->connect_response.valid() && reply->connect_response.modified()) {
                    const auto handshake = reply->connect_response.value();
                    state.connected_with(scalar_cast<i64>(handshake[0]).value());
                }
                if (reply->messages.valid() && reply->messages.modified()) {
                    const auto messages = reply->messages.value_view();
                    for (auto index = std::size_t{}; index < messages.size(); ++index) state.receive(messages[index]);
                }
            })(response);
            (void)node([&state, secure](In<TS<i64>> port) {
                if (!port.valid() || !port.modified()) return;
                const auto request = make_connect_request(static_cast<std::uint16_t>(*port), secure);
                if (!state.writer.write(literal::TimeSeriesLiteral{{"connect_request", request.view()}})) {
                    throw std::runtime_error{"WebSocket connection request queue rejected the request"};
                }
            })(http_server());
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = GraphTimeout,
                        .stop_token = state.stop.get_token()});

    if (!state.done || !state.connected) throw std::runtime_error{"WebSocket benchmark graph stopped early"};
    return Result{secure ? "wss" : "ws", "standalone_anonymous_bg_client_bg_server", payload_bytes, window,
                  std::move(state.samples)};
}

[[nodiscard]] Result run_beast_client(const Options &options, bool persistent) {
    const auto payload_bytes = std::size_t{64};
    auto catalogue = ServiceCatalogue{};
    register_http_server(catalogue);
    auto peer_ports = std::vector<std::set<std::uint16_t>>(options.samples);
    const auto handler = node([&](In<TS<Bundle<HttpServerRequest>>> request,
                                  Out<TS<Bundle<HttpResponse>>> response) {
        const auto body = scalar_cast<bytes>(request.value()[5]).value();
        const auto sequence = payload_sequence(body);
        const auto peer = scalar_cast<Bundle<HttpPeer>>(request.value()[6]);
        if (sequence > options.warmup_messages) {
            const auto sample = (sequence - options.warmup_messages - 1) / options.messages_per_sample;
            if (sample < peer_ports.size()) {
                peer_ports[sample].insert(static_cast<std::uint16_t>(scalar_cast<i64>(peer[1]).value()));
            }
        }
        const auto reply = make_http_response(request.value()[5]);
        response.set_value(reply.view());
    });
    const auto route = http_handler("/echo", handler);
    auto result = Result{"http", persistent ? "raw_beast_client_persistent" : "raw_beast_client_connection_close",
                         payload_bytes, 1, {}};
    auto failure = std::exception_ptr{};
    auto stop = std::stop_source{};
    auto client = std::jthread{};

    run_graph(
        [&] {
            (void)route();
            (void)node([&](In<TS<i64>> port) {
                if (!port.valid() || !port.modified() || client.joinable()) return;
                const auto server_port = static_cast<std::uint16_t>(*port);
                client = std::jthread{[&, server_port] {
                    try {
                        auto io = asio::io_context{};
                        auto sequence = std::uint64_t{1};
                        auto stream = std::optional<beast::tcp_stream>{};
                        auto buffer = beast::flat_buffer{};
                        for (auto phase = std::size_t{}; phase < options.samples + 1; ++phase) {
                            const auto count = phase == 0 ? options.warmup_messages : options.messages_per_sample;
                            auto sample = Sample{};
                            auto sample_started = Clock::time_point{};
                            for (auto message = std::size_t{}; message < count; ++message, ++sequence) {
                                const auto payload = make_payload(payload_bytes, sequence, 0);
                                const auto body = std::string{reinterpret_cast<const char *>(payload.data()),
                                                              payload.size()};
                                const auto started = Clock::now();
                                if (phase != 0 && message == 0) sample_started = started;
                                if (!stream) {
                                    stream.emplace(io);
                                    stream->connect({asio::ip::make_address("127.0.0.1"), server_port});
                                }
                                auto request = http::request<http::string_body>{http::verb::post, "/echo", 11};
                                request.set(http::field::host, "127.0.0.1");
                                request.keep_alive(persistent);
                                request.body() = body;
                                request.prepare_payload();
                                http::write(*stream, request);
                                auto response = http::response<http::string_body>{};
                                http::read(*stream, buffer, response);
                                if (response.result() != http::status::ok || response.body() != body) {
                                    throw std::runtime_error{"raw Beast control response validation failed"};
                                }
                                if (phase != 0) {
                                    sample.latency_ns.push_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
                                            .count());
                                }
                                if (!persistent) {
                                    boost::system::error_code error;
                                    stream->socket().shutdown(tcp::socket::shutdown_both, error);
                                    stream->socket().close(error);
                                    stream.reset();
                                }
                            }
                            if (phase != 0) {
                                sample.elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                        Clock::now() - sample_started)
                                                        .count();
                                result.samples.push_back(std::move(sample));
                            }
                        }
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    stop.request_stop();
                }};
            })(http_server());
        },
        GraphRunOptions{.services = &catalogue,
                        .engine = EvaluationEngine::realtime,
                        .run_time = GraphTimeout,
                        .stop_token = stop.get_token()});

    if (client.joinable()) client.join();
    if (failure) std::rethrow_exception(failure);
    if (result.samples.size() != options.samples) {
        throw std::runtime_error{"raw Beast client stopped before all samples completed"};
    }
    for (auto index = std::size_t{}; index < result.samples.size(); ++index) {
        result.samples[index].peer_ports = peer_ports[index].size();
    }
    return result;
}

void write_result(std::ostream &output, const Result &result) {
    for (auto index = std::size_t{}; index < result.samples.size(); ++index) write_sample(output, result, index);
}

} // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto selected_protocols = options.protocols.empty()
                                            ? std::vector<std::string>{"http", "https", "ws", "wss"}
                                            : options.protocols;
        auto file = std::ofstream{};
        auto *output = &std::cout;
        if (options.output) {
            file.open(*options.output);
            if (!file) throw std::runtime_error{"could not open benchmark output file"};
            output = &file;
        }
        write_metadata(*output, options);
        output->flush();
        auto run_raw_beast = options.protocols.empty() && options.windows.empty() && options.payload_sizes.empty();
        const auto default_payload_sizes = std::vector<std::size_t>{64, 4096};
        const auto &payload_sizes = options.payload_sizes.empty() ? default_payload_sizes : options.payload_sizes;
        for (const auto payload_bytes : payload_sizes) {
            for (const auto &protocol : selected_protocols) {
                const auto http_protocol = protocol == "http" || protocol == "https";
                const auto default_windows = http_protocol ? std::vector<std::size_t>{1, ClientCount}
                                                           : std::vector<std::size_t>{1, 64};
                const auto &windows = options.windows.empty() ? default_windows : options.windows;
                for (const auto window : windows) {
                    std::cerr << protocol << " payload=" << payload_bytes << " window=" << window << '\n';
                    if (http_protocol) {
                        write_result(*output, run_http(options, protocol, payload_bytes, window));
                    } else {
                        write_result(*output,
                                     run_websocket(options, payload_bytes, window, protocol == "wss"));
                    }
                }
            }
        }
        if (run_raw_beast) {
            std::cerr << "raw Beast client persistent\n";
            write_result(*output, run_beast_client(options, true));
            std::cerr << "raw Beast client connection-close\n";
            write_result(*output, run_beast_client(options, false));
        }
        output->flush();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "web extension benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
