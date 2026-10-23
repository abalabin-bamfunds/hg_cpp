#include "server_runtime.hpp"

#include <bg/literal/scalar_literal.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/scalar/bundle/scalar.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/system/system_error.hpp>

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
#include <vector>

namespace bg::web {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Literal = literal::ScalarLiteral;
using RequestHeaders = std::vector<std::pair<std::string, std::string>>;
using HeaderPair = Tuple<string, string>;
using namespace bg::literals;

constexpr auto request_body_limit = std::uint64_t{8 * 1024 * 1024};
constexpr auto request_timeout = std::chrono::seconds{30};

struct ParsedTarget {
    std::string path;
    std::vector<std::pair<std::string, std::string>> query;
};

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::optional<std::string> decode_component(std::string_view input, bool plus_as_space) {
    auto result = std::string{};
    result.reserve(input.size());
    for (auto index = std::size_t{}; index < input.size(); ++index) {
        if (input[index] == '%') {
            if (index + 2 >= input.size()) {
                return std::nullopt;
            }
            const auto high = hex_value(input[index + 1]);
            const auto low = hex_value(input[index + 2]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            result.push_back(static_cast<char>(high * 16 + low));
            index += 2;
        } else if (plus_as_space && input[index] == '+') {
            result.push_back(' ');
        } else {
            result.push_back(input[index]);
        }
    }
    return result;
}

std::optional<ParsedTarget> parse_target(std::string_view target) {
    if (target.empty() || target.front() != '/' || target.find('#') != std::string_view::npos) {
        return std::nullopt;
    }

    const auto query_start = target.find('?');
    const auto raw_path = target.substr(0, query_start);
    auto path = decode_component(raw_path, false);
    if (!path) {
        return std::nullopt;
    }

    auto result = ParsedTarget{.path = std::move(*path)};
    if (query_start == std::string_view::npos || query_start + 1 == target.size()) {
        return result;
    }

    auto remaining = target.substr(query_start + 1);
    while (!remaining.empty()) {
        const auto separator = remaining.find('&');
        const auto field = remaining.substr(0, separator);
        const auto equals = field.find('=');
        const auto raw_key = field.substr(0, equals);
        const auto raw_value = equals == std::string_view::npos ? std::string_view{} : field.substr(equals + 1);
        auto key = decode_component(raw_key, true);
        auto value = decode_component(raw_value, true);
        if (!key || !value) {
            return std::nullopt;
        }
        result.query.emplace_back(std::move(*key), std::move(*value));
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    return result;
}

void append_latin1_as_utf8(std::string &result, std::string_view value) {
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character < 0x80) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back(static_cast<char>(0xc0 | (character >> 6)));
            result.push_back(static_cast<char>(0x80 | (character & 0x3f)));
        }
    }
}

std::optional<std::string> utf8_as_latin1(std::string_view value) {
    auto result = std::string{};
    result.reserve(value.size());
    for (auto index = std::size_t{}; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        auto code_point = std::uint32_t{};
        auto length = std::size_t{};
        if (first < 0x80) {
            code_point = first;
            length = 1;
        } else if (first == 0xc2 || first == 0xc3) {
            if (index + 1 >= value.size()) {
                return std::nullopt;
            }
            const auto second = static_cast<unsigned char>(value[index + 1]);
            if ((second & 0xc0) != 0x80) {
                return std::nullopt;
            }
            code_point = ((first & 0x1f) << 6) | (second & 0x3f);
            length = 2;
        } else {
            return std::nullopt;
        }
        if (code_point == '\r' || code_point == '\n') {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(code_point));
        index += length;
    }
    return result;
}

bool valid_header_name(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::ranges::all_of(value, [](unsigned char character) {
        if ((character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z')) {
            return true;
        }
        constexpr auto punctuation = std::string_view{"!#$%&'*+-.^_`|~"};
        return punctuation.find(static_cast<char>(character)) != std::string_view::npos;
    });
}

ScalarValue<List<HeaderPair>> header_values(const RequestHeaders &headers) {
    auto pair_values = std::vector<ScalarValue<HeaderPair>>{};
    pair_values.reserve(headers.size());
    auto pair_views = std::vector<Scalar<HeaderPair>>{};
    pair_views.reserve(headers.size());
    for (const auto &[name, value] : headers) {
        pair_values.push_back(ScalarValue<HeaderPair>::from_literal(Literal{name, value}));
        pair_views.push_back(pair_values.back().view());
    }
    return ScalarValue<List<HeaderPair>>{pair_views};
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
        auto value = std::string{};
        append_latin1_as_utf8(value, raw_value);
        headers.emplace_back(std::move(name), std::move(value));
    }

    auto query_values = header_values(target.query);
    auto header_list = header_values(headers);
    const auto body = std::span<const std::byte>{reinterpret_cast<const std::byte *>(request.body().data()),
                                                 request.body().size()};
    auto method = ScalarValue<string>{std::string_view{request.method_string().data(),
                                                       request.method_string().size()}};
    auto raw_target = ScalarValue<string>{std::string_view{request.target().data(), request.target().size()}};
    auto path = ScalarValue<string>{target.path};
    auto body_value = ScalarValue<bytes>{body};
    auto address = ScalarValue<string>{peer.address().to_string()};
    auto peer_port = ScalarValue<i64>{static_cast<i64>(peer.port())};
    auto memory = allocator{};
    auto peer_value = bundle<HttpPeer>(memory, "address"_n = address.view(), "port"_n = peer_port.view());
    return bundle<HttpServerRequest>(memory, "method"_n = method.view(), "target"_n = raw_target.view(),
                                     "path"_n = path.view(), "query"_n = query_values.view(),
                                     "headers"_n = header_list.view(), "body"_n = body_value.view(),
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
    void write_response(ScalarValue<Bundle<HttpResponse>> response);
    void await_response(i64 request_id);
    void send_error(http::status status, std::string body, bool close);

  private:
    void read_request();
    void on_header(beast::error_code error);
    void read_body();
    void on_read(beast::error_code error);
    void watch_disconnect();
    void cancel_disconnect_watch() noexcept;
    void cancel_timeout() noexcept;
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
    bool timeout_pending_{};
};

} // namespace

class ServerImplementation final {
  public:
    ServerImplementation(ServerOptions options, std::vector<std::string> routes, Queue output)
        : output_{std::move(output)}, acceptor_{io_} {
        for (auto &route : routes) {
            routes_.insert(std::move(route));
        }
        const auto address = asio::ip::make_address(options.host);
        const auto endpoint = tcp::endpoint{address, options.port};
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address{true});
        acceptor_.bind(endpoint);
        acceptor_.listen(asio::socket_base::max_listen_connections);
        port_ = acceptor_.local_endpoint().port();
        accept();
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

    void respond(i64 id, ScalarValue<Bundle<HttpResponse>> response) {
        if (stopped_.load(std::memory_order_acquire)) {
            return;
        }
        asio::post(io_, [this, id, response = std::move(response)]() mutable {
            const auto found = pending_.find(id);
            if (found == pending_.end()) {
                return;
            }
            const auto session = found->second.lock();
            if (session) {
                session->write_response(std::move(response));
            }
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
        sessions_.clear();
        pending_.clear();
    }

    void finish_request(i64 id) {
        const auto found = pending_.find(id);
        if (found == pending_.end()) {
            return;
        }
        pending_.erase(found);
        try {
            (void)output_.write(
                ::literal::TimeSeriesLiteral{{util::symbol{"requests"}, {{id, ::literal::REMOVE}}}});
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void session_finished(const HttpSession *session) {
        std::erase_if(sessions_, [session](const auto &entry) { return entry.get() == session; });
    }

    void enqueue(const std::shared_ptr<HttpSession> &session,
                 const http::request<http::string_body> &request, const ParsedTarget &target,
                 const tcp::endpoint &peer, const RequestHeaders &request_headers) {
        if (stopped_.load(std::memory_order_acquire)) {
            session->close_for_shutdown();
            return;
        }
        if (!routes_.contains(target.path)) {
            session->send_error(http::status::not_found, "Not Found", !request.keep_alive());
            return;
        }
        if (next_id_ == 0 || next_id_ > static_cast<std::uint64_t>(std::numeric_limits<i64>::max())) {
            next_id_ = 1;
        }
        const auto id = static_cast<i64>(next_id_++);
        pending_.emplace(id, session);
        try {
            auto value = request_value(request, target, peer, request_headers);
            if (!output_.write(
                    ::literal::TimeSeriesLiteral{{util::symbol{"requests"}, {{id, value.view()}}}})) {
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

  private:
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

    std::unordered_set<std::string> routes_;
    Queue output_;
    asio::io_context io_{1};
    tcp::acceptor acceptor_;
    std::jthread thread_;
    std::uint16_t port_{};
    std::uint64_t next_id_{1};
    std::unordered_map<i64, std::weak_ptr<HttpSession>> pending_;
    std::vector<std::shared_ptr<HttpSession>> sessions_;
    std::atomic<bool> stopped_{};
    std::exception_ptr failure_;
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
    timeout_pending_ = false;
    parser_.emplace();
    parser_->body_limit(request_body_limit);
    cancel_timeout();
    timeout_.expires_after(request_timeout);
    auto self = shared_from_this();
    const auto generation = request_generation_;
    timeout_.async_wait([self, generation](beast::error_code error) {
        if (!error && generation == self->request_generation_ && !self->response_started_) {
            if (self->interim_response_pending_) {
                self->timeout_pending_ = true;
            } else {
                self->send_error(http::status::request_timeout, "Request Timeout", true);
            }
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
        if (self->timeout_pending_) {
            self->send_error(http::status::request_timeout, "Request Timeout", true);
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
        if (!valid_header_name(name)) {
            send_error(http::status::internal_server_error, "Invalid Response Header", true);
            return;
        }
        const auto latin1 = utf8_as_latin1(header_value);
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
        } else if (std::ranges::any_of(*latin1, [](unsigned char character) {
                       return (character < 0x20 && character != '\t') || character == 0x7f;
                   })) {
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
    cancel_timeout();
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

} // namespace

ServerRuntime::ServerRuntime(ServerOptions options, std::vector<std::string> routes, Queue output)
    : implementation_{std::make_unique<ServerImplementation>(std::move(options), std::move(routes),
                                                              std::move(output))} {}

ServerRuntime::~ServerRuntime() = default;

std::uint16_t ServerRuntime::port() const noexcept { return implementation_->port(); }

void ServerRuntime::respond(i64 request_id, ScalarValue<Bundle<HttpResponse>> response) {
    implementation_->respond(request_id, std::move(response));
}

} // namespace bg::web
