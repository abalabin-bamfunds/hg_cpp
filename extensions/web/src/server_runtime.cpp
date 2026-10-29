#include "server_runtime.hpp"
#include "http_header.hpp"

#include <bg/literal/time_series_literal.hpp>
#include <bg/scalar/bundle/scalar.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url.hpp>

#include <unicode/utf8.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace bg::web {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using RequestHeaders = std::vector<std::pair<std::string, std::string>>;
using namespace bg::literals;

constexpr auto request_body_limit = std::uint64_t{8 * 1024 * 1024};
constexpr auto request_timeout = std::chrono::seconds{30};

struct ParsedTarget {
    std::string path;
    std::vector<std::pair<std::string, std::string>> query;
};

std::optional<ParsedTarget> parse_target(std::string_view target) {
    const auto parsed = boost::urls::parse_origin_form(boost::core::string_view{target.data(), target.size()});
    if (!parsed) {
        return std::nullopt;
    }

    auto result = ParsedTarget{.path = parsed->path()};
    for (const auto parameter : parsed->params()) {
        result.query.emplace_back(parameter.key, parameter.value);
    }
    return result;
}

class OrderedRequestParser final : public http::request_parser<http::string_body> {
  public:
    const RequestHeaders &ordered_headers() const noexcept { return ordered_headers_; }

    void on_field_impl(http::field name, beast::string_view name_string, beast::string_view value,
                       beast::error_code &error) override {
        get().insert(name, name_string, value, error);
        if (!error) {
            ordered_headers_.emplace_back(std::string{name_string}, std::string{value});
        }
    }

  private:
    RequestHeaders ordered_headers_;
};

ScalarValue<Bundle<HttpServerRequest>> request_value(const http::request<http::string_body> &request,
                                                      const ParsedTarget &target, const tcp::endpoint &peer,
                                                      const RequestHeaders &request_headers) {
    auto headers = std::vector<std::pair<std::string, std::string>>{};
    headers.reserve(request_headers.size());
    for (const auto &[name, raw_value] : request_headers) {
        headers.emplace_back(name, details::latin1_header_value_as_utf8(raw_value));
    }

    const auto body = std::span<const std::byte>{reinterpret_cast<const std::byte *>(request.body().data()),
                                                 request.body().size()};
    const auto address = peer.address().to_string();
    const auto port = static_cast<i64>(peer.port());
    auto memory = allocator{};
    const auto peer_value = bundle<HttpPeer>(memory, "address"_n = std::string_view{address}, "port"_n = port);
    return bundle<HttpServerRequest>(
        memory,
        "method"_n = std::string_view{request.method_string().data(), request.method_string().size()},
        "target"_n = std::string_view{request.target().data(), request.target().size()},
        "path"_n = std::string_view{target.path},
        "query"_n = std::span<const std::pair<std::string, std::string>>{target.query},
        "headers"_n = std::span<const std::pair<std::string, std::string>>{headers}, "body"_n = body,
        "peer"_n = peer_value.view());
}

struct ActiveResponse {
    explicit ActiveResponse(ScalarValue<Bundle<HttpResponse>> value) : value{std::move(value)} {}

    ScalarValue<Bundle<HttpResponse>> value;
    std::string header;
};

class HttpSession final : public std::enable_shared_from_this<HttpSession> {
  public:
    HttpSession(tcp::socket socket, ServerImplementation &server)
        : socket_{std::move(socket)}, server_{&server}, timeout_{socket_.get_executor()} {}

    void run();
    void close_for_shutdown() noexcept;
    void cancel() noexcept;
    void write_response(ScalarValue<Bundle<HttpResponse>> response);
    void await_response(i64 request_id);
    void send_error(http::status status, std::string body, bool close);
    tcp::socket take_socket() noexcept { return std::move(socket_); }

  private:
    void read_request();
    void on_header(beast::error_code error);
    void read_body();
    void on_read(beast::error_code error);
    void watch_disconnect();
    void cancel_disconnect_watch() noexcept;
    void cancel_timeout() noexcept;
    void start_response_write_timeout();
    void finish_request();
    void close_connection() noexcept;
    tcp::socket socket_;
    ServerImplementation *server_;
    beast::flat_buffer buffer_;
    std::optional<OrderedRequestParser> parser_;
    asio::steady_timer timeout_;
    std::array<std::byte, 1> peek_buffer_{};
    i64 request_id_{};
    std::uint64_t request_generation_{};
    bool close_after_response_{};
    bool response_started_{};
    bool disconnect_watch_started_{};
    bool head_request_{};
    bool interim_response_pending_{};
};

class WebSocketSession final : public std::enable_shared_from_this<WebSocketSession> {
  public:
    WebSocketSession(tcp::socket socket, ServerImplementation &server, service_client_id_t client_id, i64 id,
                     http::request<http::string_body> request);

    service_client_id_t client_id() const noexcept { return client_id_; }
    void await_response();
    void apply_update(WebSocketResponseUpdate update);
    void close_for_route_removal();
    void close_for_shutdown() noexcept;

  private:
    struct MessageOperation {
        ScalarValue<bytes> value;
    };
    struct CloseOperation {
        websocket::close_reason reason;
    };
    using Operation = std::variant<MessageOperation, CloseOperation>;
    enum class State { awaiting_response, accepting, open, closing, rejecting, closed };

    void accept(ScalarValue<Bundle<HttpResponse>> response);
    void reject(ScalarValue<Bundle<HttpResponse>> response);
    void reject_with_error(http::status status, std::string body);
    void read_message();
    void enqueue_message(ScalarValue<bytes> message);
    void enqueue_close(ScalarValue<Bundle<WebSocketClose>> close);
    void run_next_operation();
    void watch_disconnect();
    void cancel_disconnect_watch() noexcept;
    void start_response_write_timeout();
    void finish() noexcept;
    void close_socket() noexcept;

    websocket::stream<tcp::socket> stream_;
    ServerImplementation *server_;
    service_client_id_t client_id_;
    i64 id_;
    http::request<http::string_body> request_;
    beast::flat_buffer read_buffer_;
    asio::steady_timer timeout_;
    std::array<std::byte, 1> peek_buffer_{};
    std::deque<Operation> operations_;
    State state_{State::awaiting_response};
    bool operation_started_{};
    bool disconnect_watch_started_{};
    bool closing_requested_{};
};

} // namespace

class ServerImplementation final {
  public:
    ServerImplementation(ServerOptions options, Queue output)
        : output_{std::move(output)}, acceptor_{io_} {
        const auto address = asio::ip::make_address(options.host);
        const auto endpoint = tcp::endpoint{address, options.port};
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address{true});
        acceptor_.bind(endpoint);
        port_ = acceptor_.local_endpoint().port();
    }

    void start() {
        if (started_) return;
        acceptor_.listen(asio::socket_base::max_listen_connections);
        accept();
        started_ = true;
        thread_ = std::jthread{[this] {
            while (!stopped_.load(std::memory_order_acquire)) {
                try {
                    io_.run();
                    return;
                } catch (...) {
                    record_failure(std::current_exception());
                }
            }
        }};
    }

    ~ServerImplementation() {
        try {
            stop();
        } catch (...) {
        }
    }

    std::uint16_t port() const noexcept { return port_; }

    void register_route(service_client_id_t client_id, std::string route) {
        if (!started_) {
            add_route(client_id, std::move(route));
            return;
        }
        asio::post(io_, [this, client_id, route = std::move(route)]() mutable {
            try {
                add_route(client_id, std::move(route));
            } catch (...) {
                record_failure(std::current_exception());
            }
        });
    }

    void attach_websocket_queue(Queue messages) {
        if (started_ || websocket_queue_attached_) {
            throw std::logic_error{"the WebSocket message queue must be attached once before the server starts"};
        }
        websocket_messages_ = std::move(messages);
        websocket_queue_attached_ = true;
    }

    void unregister_route(service_client_id_t client_id) {
        if (!started_) {
            remove_route(client_id);
            return;
        }
        if (!stopped_.load(std::memory_order_acquire)) {
            asio::post(io_, [this, client_id] { remove_route(client_id); });
        }
    }

    void register_websocket_route(service_client_id_t client_id, std::string route) {
        if (!started_) {
            add_websocket_route(client_id, std::move(route));
            return;
        }
        asio::post(io_, [this, client_id, route = std::move(route)]() mutable {
            try {
                add_websocket_route(client_id, std::move(route));
            } catch (...) {
                record_failure(std::current_exception());
            }
        });
    }

    void unregister_websocket_route(service_client_id_t client_id) {
        if (!started_) {
            remove_websocket_route(client_id);
            return;
        }
        if (!stopped_.load(std::memory_order_acquire)) {
            asio::post(io_, [this, client_id] { remove_websocket_route(client_id); });
        }
    }

    void respond(service_client_id_t client_id, i64 id, ScalarValue<Bundle<HttpResponse>> response) {
        if (stopped_.load(std::memory_order_acquire)) {
            return;
        }
        asio::post(io_, [this, client_id, id, response = std::move(response)]() mutable {
            const auto found = pending_.find(id);
            if (found == pending_.end() || found->second.client_id != client_id) {
                return;
            }
            const auto session = found->second.session.lock();
            if (session) {
                session->write_response(std::move(response));
            }
        });
    }

    void respond_websocket(service_client_id_t client_id, i64 id, WebSocketResponseUpdate update) {
        if (stopped_.load(std::memory_order_acquire)) {
            return;
        }
        asio::post(io_, [this, client_id, id, update = std::move(update)]() mutable {
            const auto found = websocket_sessions_.find(id);
            if (found == websocket_sessions_.end() || found->second->client_id() != client_id) {
                return;
            }
            const auto session = found->second;
            session->apply_update(std::move(update));
        });
    }

    void stop() {
        if (stopped_.exchange(true, std::memory_order_acq_rel)) {
            if (thread_.joinable()) {
                thread_.join();
            }
            return;
        }
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        beast::error_code ignored;
        acceptor_.close(ignored);
        for (const auto &session : sessions_) {
            session->close_for_shutdown();
        }
        while (!websocket_sessions_.empty()) {
            const auto session = websocket_sessions_.begin()->second;
            session->close_for_shutdown();
        }
        sessions_.clear();
        pending_.clear();
        websocket_sessions_.clear();
    }

    void finish_request(i64 id) {
        const auto found = pending_.find(id);
        if (found == pending_.end()) {
            return;
        }
        const auto client_id = found->second.client_id;
        pending_.erase(found);
        try {
            (void)output_.write(
                ::literal::TimeSeriesLiteral{{util::symbol{"requests"}, {{client_id, {{id, ::literal::REMOVE}}}}}});
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void session_finished(const HttpSession *session) {
        std::erase_if(sessions_, [session](const auto &entry) { return entry.get() == session; });
    }

    void finish_websocket(i64 id) noexcept {
        const auto found = websocket_sessions_.find(id);
        if (found == websocket_sessions_.end()) {
            return;
        }
        const auto client_id = found->second->client_id();
        websocket_sessions_.erase(found);
        try {
            if (websocket_queue_attached_) {
                (void)websocket_messages_.write(::literal::TimeSeriesLiteral{{client_id, {{id, ::literal::REMOVE}}}});
            }
            (void)output_.write(::literal::TimeSeriesLiteral{
                {util::symbol{"websocket_requests"}, {{client_id, {{id, ::literal::REMOVE}}}}}});
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void enqueue(const std::shared_ptr<HttpSession> &session,
                 const http::request<http::string_body> &request, const ParsedTarget &target,
                 const tcp::endpoint &peer, const RequestHeaders &request_headers) {
        if (stopped_.load(std::memory_order_acquire)) {
            session->close_for_shutdown();
            return;
        }
        const auto route = routes_.find(target.path);
        if (route == routes_.end()) {
            session->send_error(http::status::not_found, "Not Found", !request.keep_alive());
            return;
        }
        const auto client_id = route->second;
        if (next_id_ == 0 || next_id_ > static_cast<std::uint64_t>(std::numeric_limits<i64>::max())) {
            next_id_ = 1;
        }
        const auto id = static_cast<i64>(next_id_++);
        pending_.emplace(id, PendingRequest{.session = session, .client_id = client_id});
        try {
            auto value = request_value(request, target, peer, request_headers);
            if (!output_.write(::literal::TimeSeriesLiteral{
                    {util::symbol{"requests"}, {{client_id, {{id, value.view()}}}}}})) {
                pending_.erase(id);
                session->send_error(http::status::service_unavailable, "Service Unavailable", true);
                return;
            }
        } catch (...) {
            pending_.erase(id);
            record_failure(std::current_exception());
            session->send_error(http::status::internal_server_error, "Internal Server Error", true);
            return;
        }
        session->await_response(id);
    }

    void enqueue_websocket(const std::shared_ptr<HttpSession> &http_session,
                           http::request<http::string_body> request, const ParsedTarget &target,
                           const tcp::endpoint &peer, const RequestHeaders &request_headers) {
        if (stopped_.load(std::memory_order_acquire)) {
            http_session->close_for_shutdown();
            return;
        }
        const auto route = websocket_routes_.find(target.path);
        if (route == websocket_routes_.end()) {
            http_session->send_error(http::status::not_found, "Not Found", true);
            return;
        }
        if (!websocket_queue_attached_) {
            http_session->send_error(http::status::service_unavailable, "Service Unavailable", true);
            return;
        }
        if (next_id_ == 0 || next_id_ > static_cast<std::uint64_t>(std::numeric_limits<i64>::max())) {
            next_id_ = 1;
        }
        const auto id = static_cast<i64>(next_id_++);
        const auto client_id = route->second;
        auto value = request_value(request, target, peer, request_headers);
        auto session = std::make_shared<WebSocketSession>(http_session->take_socket(), *this, client_id, id,
                                                         std::move(request));
        session_finished(http_session.get());
        websocket_sessions_.emplace(id, session);
        try {
            if (!output_.write(::literal::TimeSeriesLiteral{
                    {util::symbol{"websocket_requests"}, {{client_id, {{id, value.view()}}}}}})) {
                session->close_for_shutdown();
                return;
            }
        } catch (...) {
            record_failure(std::current_exception());
            session->close_for_shutdown();
            return;
        }
        session->await_response();
    }

    void enqueue_websocket_message(i64 id, std::span<const std::byte> message) noexcept {
        const auto found = websocket_sessions_.find(id);
        if (found == websocket_sessions_.end() || !websocket_queue_attached_) {
            return;
        }
        try {
            if (!websocket_messages_.write(::literal::TimeSeriesLiteral{
                    {found->second->client_id(), {{id, literal::ScalarLiteral(message)}}}})) {
                found->second->close_for_shutdown();
            }
        } catch (...) {
            record_failure(std::current_exception());
            found->second->close_for_shutdown();
        }
    }

  private:
    struct PendingRequest {
        std::weak_ptr<HttpSession> session;
        service_client_id_t client_id;
    };

    void add_route(service_client_id_t client_id, std::string route) {
        const auto existing_client = route_by_client_.find(client_id);
        if (existing_client != route_by_client_.end()) {
            if (existing_client->second != route) {
                throw std::logic_error{"an HTTP handler route changed while its client was active"};
            }
            return;
        }
        if (routes_.contains(route)) {
            throw std::invalid_argument{"an HTTP route is already registered"};
        }
        routes_.emplace(route, client_id);
        route_by_client_.emplace(client_id, std::move(route));
    }

    void remove_route(service_client_id_t client_id) {
        const auto found = route_by_client_.find(client_id);
        if (found == route_by_client_.end()) return;
        routes_.erase(found->second);
        route_by_client_.erase(found);

        for (auto pending = pending_.begin(); pending != pending_.end();) {
            if (pending->second.client_id != client_id) {
                ++pending;
                continue;
            }
            const auto id = pending->first;
            const auto session = pending->second.session.lock();
            ++pending;
            finish_request(id);
            if (session) session->cancel();
        }

        try {
            (void)output_.write(
                ::literal::TimeSeriesLiteral{{util::symbol{"requests"}, {{client_id, ::literal::REMOVE}}}});
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void add_websocket_route(service_client_id_t client_id, std::string route) {
        const auto existing_client = websocket_route_by_client_.find(client_id);
        if (existing_client != websocket_route_by_client_.end()) {
            if (existing_client->second != route) {
                throw std::logic_error{"a WebSocket handler route changed while its client was active"};
            }
            return;
        }
        if (websocket_routes_.contains(route)) {
            throw std::invalid_argument{"a WebSocket route is already registered"};
        }
        websocket_routes_.emplace(route, client_id);
        websocket_route_by_client_.emplace(client_id, std::move(route));
    }

    void remove_websocket_route(service_client_id_t client_id) {
        const auto found = websocket_route_by_client_.find(client_id);
        if (found == websocket_route_by_client_.end()) {
            return;
        }
        websocket_routes_.erase(found->second);
        websocket_route_by_client_.erase(found);

        auto sessions = std::vector<std::shared_ptr<WebSocketSession>>{};
        for (const auto &[id, session] : websocket_sessions_) {
            (void)id;
            if (session->client_id() == client_id) {
                sessions.push_back(session);
            }
        }
        for (const auto &session : sessions) {
            session->close_for_route_removal();
        }

        try {
            if (websocket_queue_attached_) {
                (void)websocket_messages_.write(::literal::TimeSeriesLiteral{{client_id, ::literal::REMOVE}});
            }
            (void)output_.write(
                ::literal::TimeSeriesLiteral{{util::symbol{"websocket_requests"}, {{client_id, ::literal::REMOVE}}}});
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void record_failure(std::exception_ptr failure) noexcept {
        try {
            auto message = std::string{"unexpected HTTP transport failure"};
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception &error) {
                message = error.what();
            } catch (...) {
            }

            if (failure_) {
                return;
            }
            failure_ = std::move(failure);
            (void)output_.write(::literal::TimeSeriesLiteral{{util::symbol{"errors"}, message}});
        } catch (...) {
        }
    }

    void accept() {
        acceptor_.async_accept(asio::make_strand(io_), [this](beast::error_code error, tcp::socket socket) {
            if (stopped_.load(std::memory_order_acquire)) {
                return;
            }
            if (error) {
                if (error == asio::error::connection_aborted) {
                    accept();
                } else {
                    record_failure(std::make_exception_ptr(boost::system::system_error{error}));
                }
                return;
            }
            auto session = std::make_shared<HttpSession>(std::move(socket), *this);
            sessions_.push_back(session);
            session->run();
            accept();
        });
    }

    std::unordered_map<std::string, service_client_id_t> routes_;
    std::unordered_map<service_client_id_t, std::string> route_by_client_;
    std::unordered_map<std::string, service_client_id_t> websocket_routes_;
    std::unordered_map<service_client_id_t, std::string> websocket_route_by_client_;
    Queue output_;
    Queue websocket_messages_;
    bool websocket_queue_attached_{};
    asio::io_context io_{1};
    tcp::acceptor acceptor_;
    std::jthread thread_;
    std::uint16_t port_{};
    std::uint64_t next_id_{1};
    std::unordered_map<i64, PendingRequest> pending_;
    std::unordered_map<i64, std::shared_ptr<WebSocketSession>> websocket_sessions_;
    std::vector<std::shared_ptr<HttpSession>> sessions_;
    std::atomic<bool> stopped_{};
    std::exception_ptr failure_;
    bool started_{};
};

namespace {

void HttpSession::run() { read_request(); }

void HttpSession::read_request() {
    cancel_disconnect_watch();
    ++request_generation_;
    close_after_response_ = false;
    response_started_ = false;
    disconnect_watch_started_ = false;
    head_request_ = false;
    interim_response_pending_ = false;
    parser_.emplace();
    parser_->body_limit(request_body_limit);
    cancel_timeout();
    timeout_.expires_after(request_timeout);
    auto self = shared_from_this();
    const auto generation = request_generation_;
    timeout_.async_wait([self, generation](beast::error_code error) {
        if (error || generation != self->request_generation_) return;
        if (self->response_started_ || self->interim_response_pending_) {
            self->close_connection();
        } else {
            self->send_error(http::status::request_timeout, "Request Timeout", true);
        }
    });
    http::async_read_header(socket_, buffer_, *parser_, [self, generation](beast::error_code error, std::size_t) {
        if (generation != self->request_generation_) {
            return;
        }
        self->on_header(error);
    });
}

void HttpSession::on_header(beast::error_code error) {
    if (response_started_) {
        return;
    }
    const auto &request = parser_->get();
    head_request_ = request.method() == http::verb::head;
    if (error) {
        on_read(error);
        return;
    }

    auto host_count = std::size_t{};
    auto host = std::string_view{};
    for (const auto &field : request) {
        if (field.name() != http::field::host) continue;
        ++host_count;
        host = {field.value().data(), field.value().size()};
    }
    while (!host.empty() && (host.front() == ' ' || host.front() == '\t')) host.remove_prefix(1);
    while (!host.empty() && (host.back() == ' ' || host.back() == '\t')) host.remove_suffix(1);
    if ((request.version() == 11 && host_count != 1) || host_count > 1) {
        send_error(http::status::bad_request, "Bad Request", true);
        return;
    }
    if (host_count == 1) {
        const auto authority = boost::urls::parse_authority(boost::core::string_view{host.data(), host.size()});
        if (!authority || authority->has_userinfo() || authority->encoded_host().empty()) {
            send_error(http::status::bad_request, "Bad Request", true);
            return;
        }
    }

    const auto expect = request.find(http::field::expect);
    if (expect != request.end()) {
        auto value = std::string_view{expect->value().data(), expect->value().size()};
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }
        if (!beast::iequals(value, "100-continue")) {
            send_error(http::status::expectation_failed, "Expectation Failed", true);
            return;
        }
    }

    if (parser_->is_done()) {
        on_read({});
        return;
    }
    if (expect == request.end()) {
        read_body();
        return;
    }

    interim_response_pending_ = true;
    auto response = std::make_shared<http::response<http::empty_body>>(http::status::continue_, 11);
    auto self = shared_from_this();
    const auto generation = request_generation_;
    http::async_write(socket_, *response, [self, response, generation](beast::error_code write_error, std::size_t) {
        if (generation != self->request_generation_) {
            return;
        }
        self->interim_response_pending_ = false;
        if (write_error) {
            self->close_connection();
            return;
        }
        self->read_body();
    });
}

void HttpSession::read_body() {
    auto self = shared_from_this();
    const auto generation = request_generation_;
    http::async_read(socket_, buffer_, *parser_, [self, generation](beast::error_code error, std::size_t) {
        if (generation != self->request_generation_) {
            return;
        }
        self->on_read(error);
    });
}

void HttpSession::on_read(beast::error_code error) {
    if (response_started_) {
        return;
    }
    cancel_timeout();
    if (error == http::error::end_of_stream || error == asio::error::eof) {
        close_connection();
        return;
    }
    if (error) {
        if (error == http::error::body_limit) {
            send_error(http::status::payload_too_large, "Payload Too Large", true);
        } else {
            close_connection();
        }
        return;
    }

    auto ordered_headers = parser_->ordered_headers();
    auto request = parser_->release();
    head_request_ = request.method() == http::verb::head;
    parser_.reset();
    auto target = parse_target({request.target().data(), request.target().size()});
    if (!target) {
        send_error(http::status::bad_request, "Bad Request", true);
        return;
    }
    beast::error_code peer_error;
    const auto peer = socket_.remote_endpoint(peer_error);
    if (peer_error) {
        close_connection();
        return;
    }
    close_after_response_ = !request.keep_alive();
    if (websocket::is_upgrade(request)) {
        if (buffer_.size() != 0) {
            send_error(http::status::bad_request, "Unexpected WebSocket Data", true);
            return;
        }
        server_->enqueue_websocket(shared_from_this(), std::move(request), *target, peer, ordered_headers);
        return;
    }
    server_->enqueue(shared_from_this(), request, *target, peer, ordered_headers);
}

void HttpSession::await_response(i64 request_id) {
    request_id_ = request_id;
    response_started_ = false;
    ++request_generation_;
    cancel_timeout();
    timeout_.expires_after(request_timeout);
    auto self = shared_from_this();
    const auto generation = request_generation_;
    timeout_.async_wait([self, generation](beast::error_code error) {
        if (!error && generation == self->request_generation_ && !self->response_started_) {
            self->send_error(http::status::gateway_timeout, "Gateway Timeout", true);
        }
    });
    watch_disconnect();
}

void HttpSession::watch_disconnect() {
    if (disconnect_watch_started_ || response_started_) {
        return;
    }
    disconnect_watch_started_ = true;
    auto self = shared_from_this();
    const auto generation = request_generation_;
    socket_.async_receive(asio::buffer(peek_buffer_), tcp::socket::message_peek,
                          [self, generation](beast::error_code error, std::size_t size) {
                              if (generation != self->request_generation_) {
                                  return;
                              }
                              self->disconnect_watch_started_ = false;
                              if (self->response_started_) {
                                  return;
                              }
                              if (error == asio::error::eof || (!error && size == 0)) {
                                  self->close_connection();
                              }
                          });
}

void HttpSession::cancel_disconnect_watch() noexcept {
    if (!disconnect_watch_started_) {
        return;
    }
    disconnect_watch_started_ = false;
    beast::error_code ignored;
    socket_.cancel(ignored);
}

void HttpSession::cancel_timeout() noexcept {
    try {
        timeout_.cancel();
    } catch (...) {
    }
}

void HttpSession::start_response_write_timeout() {
    cancel_timeout();
    timeout_.expires_after(request_timeout);
    auto self = shared_from_this();
    const auto generation = request_generation_;
    timeout_.async_wait([self, generation](beast::error_code error) {
        if (!error && generation == self->request_generation_ && self->response_started_) {
            self->close_connection();
        }
    });
}

void HttpSession::send_error(http::status status, std::string body, bool close) {
    if (response_started_) {
        return;
    }
    response_started_ = true;
    cancel_disconnect_watch();
    cancel_timeout();
    if (request_id_ != 0) {
        finish_request();
    }
    start_response_write_timeout();
    close_after_response_ = close;
    auto response = std::make_shared<http::response<http::string_body>>(status, 11);
    response->set(http::field::content_type, "text/plain; charset=utf-8");
    const auto body_size = body.size();
    if (!head_request_) {
        response->body() = std::move(body);
    }
    response->prepare_payload();
    if (head_request_) {
        response->content_length(body_size);
    }
    response->keep_alive(!close);
    auto self = shared_from_this();
    http::async_write(socket_, *response, [self, response](beast::error_code error, std::size_t) {
        if (error || self->close_after_response_) {
            self->close_connection();
            return;
        }
        self->read_request();
    });
}

void HttpSession::write_response(ScalarValue<Bundle<HttpResponse>> retained_response) {
    if (response_started_ || request_id_ == 0) {
        return;
    }
    auto active = std::make_shared<ActiveResponse>(std::move(retained_response));
    const auto value = active->value.view();
    const auto status_scalar = scalar_cast<i64>(value[0]);
    auto status = status_scalar.value();
    const auto body_field = value[2];
    const auto body_scalar = Scalar<bytes>::from(body_field);
    const auto body = body_scalar.value();
    if (status < 100 || status > 599) {
        status = 500;
    }

    const auto headers_field = value[1];
    auto headers = Scalar<List<Tuple<string, string>>>::from(headers_field);
    auto ordered_headers = std::vector<std::pair<std::string, std::string>>{};
    ordered_headers.reserve(headers.size());
    auto content_length_seen = false;
    for (auto index = std::size_t{}; index < headers.size(); ++index) {
        const auto header = headers[index];
        const auto name_view = header.template get<0>().value();
        const auto value_view = header.template get<1>().value();
        const auto name = std::string_view{name_view};
        const auto header_value = std::string_view{value_view};
        if (!details::valid_http_header_name(name)) {
            send_error(http::status::internal_server_error, "Invalid Response Header", true);
            return;
        }
        const auto latin1 = details::utf8_header_value_to_latin1(header_value);
        if (!latin1) {
            send_error(http::status::internal_server_error, "Invalid Response Header", true);
            return;
        }
        if (beast::iequals(name, "content-length")) {
            if (content_length_seen) {
                send_error(http::status::internal_server_error, "Invalid Response Header", true);
                return;
            }
            content_length_seen = true;
            auto declared_length = std::uint64_t{};
            const auto [end, error] = std::from_chars(header_value.data(), header_value.data() + header_value.size(),
                                                       declared_length);
            if (error != std::errc{} || end != header_value.data() + header_value.size()) {
                send_error(http::status::internal_server_error, "Invalid Response Header", true);
                return;
            }
            if (declared_length != body.size()) {
                send_error(http::status::internal_server_error, "Invalid Response Header", true);
                return;
            }
        } else if (beast::iequals(name, "transfer-encoding") || beast::iequals(name, "connection")) {
            send_error(http::status::internal_server_error, "Invalid Response Header", true);
            return;
        }
        ordered_headers.emplace_back(name, *latin1);
    }

    if (status >= 100 && status < 200) {
        send_error(http::status::internal_server_error, "Internal Server Error", true);
        return;
    }

    response_started_ = true;
    cancel_disconnect_watch();
    start_response_write_timeout();
    const auto status_without_body = status == 204 || status == 205;
    const auto no_transmitted_body = status_without_body || status == 304 || head_request_;

    const auto code = http::status{static_cast<unsigned>(status)};
    const auto reason = http::obsolete_reason(code);
    active->header.reserve(128);
    active->header.append("HTTP/1.1 ");
    active->header.append(std::to_string(status));
    active->header.push_back(' ');
    active->header.append(reason.data(), reason.size());
    active->header.append("\r\n");
    for (const auto &[name, header_value] : ordered_headers) {
        if (status == 204 && beast::iequals(name, "content-length")) {
            continue;
        }
        active->header.append(name);
        active->header.append(": ");
        if (status == 205 && beast::iequals(name, "content-length")) {
            active->header.push_back('0');
        } else {
            active->header.append(header_value);
        }
        active->header.append("\r\n");
    }
    if (!content_length_seen && status != 204) {
        active->header.append("Content-Length: ");
        active->header.append(std::to_string(status == 205 ? 0 : body.size()));
        active->header.append("\r\n");
    }
    if (close_after_response_) {
        active->header.append("Connection: close\r\n");
    }
    active->header.append("\r\n");

    const auto transmitted_size = no_transmitted_body ? std::size_t{} : body.size();
    auto self = shared_from_this();
    const auto response_buffers = std::array<asio::const_buffer, 2>{
        asio::const_buffer{active->header.data(), active->header.size()},
        asio::const_buffer{body.data(), transmitted_size}};
    asio::async_write(socket_, response_buffers, [self, active](beast::error_code error, std::size_t) {
        self->finish_request();
        if (error || self->close_after_response_) {
            self->close_connection();
            return;
        }
        self->read_request();
    });
}

void HttpSession::finish_request() {
    ++request_generation_;
    cancel_timeout();
    if (request_id_ != 0) {
        server_->finish_request(std::exchange(request_id_, 0));
    }
}

void HttpSession::close_connection() noexcept {
    finish_request();
    beast::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    server_->session_finished(this);
}

void HttpSession::close_for_shutdown() noexcept {
    beast::error_code ignored;
    cancel_timeout();
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

void HttpSession::cancel() noexcept { close_connection(); }

WebSocketSession::WebSocketSession(tcp::socket socket, ServerImplementation &server, service_client_id_t client_id,
                                   i64 id, http::request<http::string_body> request)
    : stream_{std::move(socket)}, server_{&server}, client_id_{client_id}, id_{id}, request_{std::move(request)},
      timeout_{stream_.get_executor()} {
    stream_.read_message_max(request_body_limit);
    stream_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
}

void WebSocketSession::await_response() {
    timeout_.expires_after(request_timeout);
    auto self = shared_from_this();
    timeout_.async_wait([self](beast::error_code error) {
        if (error || self->state_ == State::closed || self->state_ == State::rejecting) {
            return;
        }
        if (self->state_ == State::awaiting_response) {
            self->reject_with_error(http::status::gateway_timeout, "Gateway Timeout");
        } else {
            self->close_socket();
        }
    });
    watch_disconnect();
}

void WebSocketSession::watch_disconnect() {
    if (disconnect_watch_started_ || state_ != State::awaiting_response) {
        return;
    }
    disconnect_watch_started_ = true;
    auto self = shared_from_this();
    stream_.next_layer().async_receive(asio::buffer(peek_buffer_), tcp::socket::message_peek,
                                       [self](beast::error_code error, std::size_t size) {
                                           self->disconnect_watch_started_ = false;
                                           if (self->state_ != State::awaiting_response) {
                                               return;
                                           }
                                           if (error == asio::error::eof || (!error && size == 0)) {
                                               self->close_socket();
                                               return;
                                           }
                                           if (!error && size != 0) {
                                               self->reject_with_error(http::status::bad_request,
                                                                       "Unexpected WebSocket Data");
                                               return;
                                           }
                                           if (error != asio::error::operation_aborted) {
                                               self->close_socket();
                                           }
                                       });
}

void WebSocketSession::cancel_disconnect_watch() noexcept {
    if (!disconnect_watch_started_) {
        return;
    }
    disconnect_watch_started_ = false;
    beast::error_code ignored;
    stream_.next_layer().cancel(ignored);
}

void WebSocketSession::start_response_write_timeout() {
    timeout_.expires_after(request_timeout);
    auto self = shared_from_this();
    timeout_.async_wait([self](beast::error_code error) {
        if (!error && self->state_ == State::rejecting) {
            self->close_socket();
        }
    });
}

void WebSocketSession::apply_update(WebSocketResponseUpdate update) {
    if (state_ == State::closed || state_ == State::rejecting || closing_requested_) {
        return;
    }

    if (update.connect_response && state_ == State::awaiting_response) {
        const auto response = update.connect_response->view();
        const auto status = scalar_cast<i64>(response[0]).value();
        if (status == 101) {
            accept(std::move(*update.connect_response));
        } else {
            reject(std::move(*update.connect_response));
        }
    }

    if (state_ == State::rejecting || state_ == State::closed) {
        return;
    }
    if (update.message) {
        enqueue_message(std::move(*update.message));
    }
    if (update.close) {
        const auto close = update.close->view();
        const auto code = scalar_cast<i64>(close[0]).value();
        if ((state_ != State::awaiting_response || code != 1008) && !closing_requested_) {
            enqueue_close(std::move(*update.close));
        }
    }
    run_next_operation();
}

void WebSocketSession::accept(ScalarValue<Bundle<HttpResponse>> response) {
    const auto value = response.view();
    const auto status = scalar_cast<i64>(value[0]).value();
    const auto body_field = value[2];
    const auto body = Scalar<bytes>::from(body_field).value();
    if (status != 101 || !body.empty()) {
        reject_with_error(http::status::internal_server_error, "Invalid WebSocket Response");
        return;
    }

    const auto headers_field = value[1];
    auto headers = Scalar<List<Tuple<string, string>>>::from(headers_field);
    auto ordered_headers = std::vector<std::pair<std::string, std::string>>{};
    ordered_headers.reserve(headers.size());
    auto offered_protocols = std::vector<std::string>{};
    const auto offered = request_.equal_range(http::field::sec_websocket_protocol);
    for (auto header = offered.first; header != offered.second; ++header) {
        auto remaining = std::string_view{header->value().data(), header->value().size()};
        while (true) {
            const auto comma = remaining.find(',');
            auto token = remaining.substr(0, comma);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
            if (!details::valid_http_header_name(token)) {
                reject_with_error(http::status::bad_request, "Invalid WebSocket Protocol Offer");
                return;
            }
            offered_protocols.emplace_back(token);
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma + 1);
        }
    }
    auto selected_protocol = false;
    for (auto index = std::size_t{}; index < headers.size(); ++index) {
        const auto header = headers[index];
        const auto name = std::string_view{header.template get<0>().value()};
        const auto header_value = std::string_view{header.template get<1>().value()};
        if (!details::valid_http_header_name(name) || beast::iequals(name, "content-length") ||
            beast::iequals(name, "transfer-encoding") || beast::iequals(name, "connection") ||
            beast::iequals(name, "upgrade") || beast::iequals(name, "sec-websocket-accept") ||
            beast::iequals(name, "sec-websocket-extensions")) {
            reject_with_error(http::status::internal_server_error, "Invalid WebSocket Response Header");
            return;
        }
        if (beast::iequals(name, "sec-websocket-protocol")) {
            if (selected_protocol || header_value.find(',') != std::string_view::npos ||
                !details::valid_http_header_name(header_value) ||
                std::ranges::find(offered_protocols, header_value) == offered_protocols.end()) {
                reject_with_error(http::status::internal_server_error, "Invalid WebSocket Subprotocol");
                return;
            }
            selected_protocol = true;
        }
        const auto latin1 = details::utf8_header_value_to_latin1(header_value);
        if (!latin1) {
            reject_with_error(http::status::internal_server_error, "Invalid WebSocket Response Header");
            return;
        }
        ordered_headers.emplace_back(name, *latin1);
    }

    cancel_disconnect_watch();
    state_ = State::accepting;
    auto self = shared_from_this();
    stream_.set_option(websocket::stream_base::decorator(
        [headers = std::move(ordered_headers)](websocket::response_type &handshake) {
            for (const auto &[name, value] : headers) {
                handshake.insert(name, value);
            }
        }));
    stream_.async_accept(request_, [self](beast::error_code error) {
        if (error) {
            self->close_socket();
            return;
        }
        self->timeout_.cancel();
        self->state_ = State::open;
        self->read_message();
        self->run_next_operation();
    });
}

void WebSocketSession::reject(ScalarValue<Bundle<HttpResponse>> response) {
    const auto value = response.view();
    const auto status = scalar_cast<i64>(value[0]).value();
    const auto body_field = value[2];
    const auto body_value = Scalar<bytes>::from(body_field).value();
    if (status < 400 || status > 599) {
        reject_with_error(http::status::internal_server_error, "Invalid WebSocket Response");
        return;
    }

    auto body = std::string{reinterpret_cast<const char *>(body_value.data()), body_value.size()};
    auto response_message = std::make_shared<http::response<http::string_body>>(
        http::status{static_cast<unsigned>(status)}, request_.version());
    response_message->body() = std::move(body);
    response_message->keep_alive(false);

    const auto headers_field = value[1];
    const auto headers = Scalar<List<Tuple<string, string>>>::from(headers_field);
    auto content_length_seen = false;
    for (auto index = std::size_t{}; index < headers.size(); ++index) {
        const auto header = headers[index];
        const auto name_view = header.template get<0>().value();
        const auto value_view = header.template get<1>().value();
        const auto name = std::string_view{name_view};
        const auto header_value = std::string_view{value_view};
        if (!details::valid_http_header_name(name)) {
            reject_with_error(http::status::internal_server_error, "Invalid WebSocket Response Header");
            return;
        }
        const auto latin1 = details::utf8_header_value_to_latin1(header_value);
        if (!latin1 || beast::iequals(name, "transfer-encoding") || beast::iequals(name, "connection")) {
            reject_with_error(http::status::internal_server_error, "Invalid WebSocket Response Header");
            return;
        }
        if (beast::iequals(name, "content-length")) {
            if (content_length_seen) {
                reject_with_error(http::status::internal_server_error, "Invalid WebSocket Response Header");
                return;
            }
            content_length_seen = true;
            auto declared_length = std::uint64_t{};
            const auto [end, error] = std::from_chars(header_value.data(), header_value.data() + header_value.size(),
                                                       declared_length);
            if (error != std::errc{} || end != header_value.data() + header_value.size() ||
                declared_length != body_value.size()) {
                reject_with_error(http::status::internal_server_error, "Invalid WebSocket Response Header");
                return;
            }
        }
        response_message->insert(name, *latin1);
    }
    response_message->prepare_payload();

    state_ = State::rejecting;
    cancel_disconnect_watch();
    start_response_write_timeout();
    auto self = shared_from_this();
    http::async_write(stream_.next_layer(), *response_message,
                      [self, response_message](beast::error_code, std::size_t) { self->close_socket(); });
}

void WebSocketSession::reject_with_error(http::status status, std::string body) {
    if (state_ == State::closed || state_ == State::rejecting) {
        return;
    }
    state_ = State::rejecting;
    cancel_disconnect_watch();
    start_response_write_timeout();
    auto response = std::make_shared<http::response<http::string_body>>(status, request_.version());
    response->set(http::field::content_type, "text/plain; charset=utf-8");
    response->body() = std::move(body);
    response->keep_alive(false);
    response->prepare_payload();
    auto self = shared_from_this();
    http::async_write(stream_.next_layer(), *response, [self, response](beast::error_code, std::size_t) {
        self->close_socket();
    });
}

void WebSocketSession::read_message() {
    if (state_ != State::open || closing_requested_) {
        return;
    }
    auto self = shared_from_this();
    stream_.async_read(read_buffer_, [self](beast::error_code error, std::size_t) {
        if (error == websocket::error::closed) {
            self->finish();
            return;
        }
        if (error == websocket::error::message_too_big) {
            self->close_socket();
            return;
        }
        if (error) {
            if (error == asio::error::operation_aborted && self->state_ == State::closing) {
                self->run_next_operation();
                return;
            }
            self->close_socket();
            return;
        }
        if (self->state_ != State::open || self->closing_requested_) {
            self->run_next_operation();
            return;
        }
        if (!self->stream_.got_binary()) {
            self->closing_requested_ = true;
            self->operations_.emplace_back(CloseOperation{websocket::close_code::unknown_data});
            self->run_next_operation();
            return;
        }

        const auto data = self->read_buffer_.data();
        const auto message = std::span<const std::byte>{static_cast<const std::byte *>(data.data()), data.size()};
        self->server_->enqueue_websocket_message(self->id_, message);
        self->read_buffer_.consume(self->read_buffer_.size());
        self->read_message();
    });
}

void WebSocketSession::enqueue_message(ScalarValue<bytes> message) {
    if (state_ == State::rejecting || state_ == State::closed || closing_requested_) {
        return;
    }
    operations_.emplace_back(MessageOperation{std::move(message)});
}

void WebSocketSession::enqueue_close(ScalarValue<Bundle<WebSocketClose>> close) {
    if (state_ == State::rejecting || state_ == State::closed) {
        return;
    }
    const auto value = close.view();
    const auto code = scalar_cast<i64>(value[0]).value();
    const auto reason_field = value[1];
    const auto reason_value = Scalar<string>::from(reason_field).value();
    auto reason = std::string{reason_value};
    const auto standard_code = code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006;
    const auto application_code = code >= 3000 && code <= 4999;
    auto reason_valid = reason.size() <= 123;
    if (reason_valid) {
        auto index = std::int32_t{};
        const auto size = static_cast<std::int32_t>(reason.size());
        const auto *data = reinterpret_cast<const std::uint8_t *>(reason.data());
        while (index < size) {
            UChar32 code_point{};
            U8_NEXT(data, index, size, code_point);
            if (code_point < 0) {
                reason_valid = false;
                break;
            }
        }
    }
    closing_requested_ = true;
    if ((!standard_code && !application_code) || !reason_valid) {
        operations_.emplace_back(CloseOperation{websocket::close_code::internal_error});
        return;
    }
    auto close_reason = websocket::close_reason{};
    close_reason.code = static_cast<websocket::close_code>(code);
    close_reason.reason = std::move(reason);
    operations_.emplace_back(CloseOperation{std::move(close_reason)});
}

void WebSocketSession::run_next_operation() {
    if ((state_ != State::open && state_ != State::closing) || operation_started_ || operations_.empty()) {
        return;
    }
    operation_started_ = true;
    auto self = shared_from_this();
    if (auto *message = std::get_if<MessageOperation>(&operations_.front())) {
        const auto payload = message->value.view().value();
        stream_.binary(true);
        stream_.async_write(asio::buffer(payload.data(), payload.size()), [self](beast::error_code error, std::size_t) {
            if (error) {
                self->close_socket();
                return;
            }
            self->operations_.pop_front();
            self->operation_started_ = false;
            self->run_next_operation();
        });
        return;
    }

    auto reason = std::get<CloseOperation>(operations_.front()).reason;
    state_ = State::closing;
    stream_.async_close(reason, [self](beast::error_code) { self->close_socket(); });
}

void WebSocketSession::close_for_route_removal() {
    if (state_ == State::awaiting_response) {
        reject_with_error(http::status::service_unavailable, "WebSocket Route Removed");
        return;
    }
    if (state_ == State::accepting) {
        closing_requested_ = true;
        operations_.emplace_back(CloseOperation{websocket::close_code::going_away});
        return;
    }
    if (state_ == State::open) {
        closing_requested_ = true;
        operations_.emplace_back(CloseOperation{websocket::close_code::going_away});
        run_next_operation();
    }
}

void WebSocketSession::close_for_shutdown() noexcept { close_socket(); }

void WebSocketSession::finish() noexcept {
    if (state_ == State::closed) {
        return;
    }
    state_ = State::closed;
    try {
        timeout_.cancel();
    } catch (...) {
    }
    cancel_disconnect_watch();
    server_->finish_websocket(id_);
}

void WebSocketSession::close_socket() noexcept {
    finish();
    beast::error_code ignored;
    stream_.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
    stream_.next_layer().close(ignored);
}

} // namespace

ServerRuntime::ServerRuntime(ServerOptions options, Queue output)
    : implementation_{std::make_unique<ServerImplementation>(std::move(options), std::move(output))} {}

ServerRuntime::~ServerRuntime() = default;

std::uint16_t ServerRuntime::port() const noexcept { return implementation_->port(); }

void ServerRuntime::attach_websocket_queue(Queue messages) {
    implementation_->attach_websocket_queue(std::move(messages));
}

void ServerRuntime::register_http_route(service_client_id_t client_id, std::string route) {
    implementation_->register_route(client_id, std::move(route));
}

void ServerRuntime::unregister_http_route(service_client_id_t client_id) {
    implementation_->unregister_route(client_id);
}

void ServerRuntime::register_websocket_route(service_client_id_t client_id, std::string route) {
    implementation_->register_websocket_route(client_id, std::move(route));
}

void ServerRuntime::unregister_websocket_route(service_client_id_t client_id) {
    implementation_->unregister_websocket_route(client_id);
}

void ServerRuntime::start() { implementation_->start(); }

void ServerRuntime::respond(service_client_id_t client_id, i64 request_id,
                            ScalarValue<Bundle<HttpResponse>> response) {
    implementation_->respond(client_id, request_id, std::move(response));
}

void ServerRuntime::respond_websocket(service_client_id_t client_id, i64 connection_id,
                                      WebSocketResponseUpdate update) {
    implementation_->respond_websocket(client_id, connection_id, std::move(update));
}

} // namespace bg::web
