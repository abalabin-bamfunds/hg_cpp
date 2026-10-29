#include "websocket_client_runtime.hpp"

#include "http_header.hpp"
#include "websocket_client_output.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/url.hpp>

#include <unicode/utf8.h>

#include <bg/literal/time_series_literal.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg::web {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace bg::literals;

using Header = Tuple<string, string>;
using HeaderValues = std::vector<std::pair<std::string, std::string>>;

struct ParsedUrl {
    std::string host;
    std::string service;
    std::string host_header;
    std::string target;
};

std::optional<std::string> parse_url(std::string_view value, ParsedUrl &parsed) {
    if (value.find('\0') != std::string_view::npos) return std::string{"WebSocket URL contains a NUL byte"};
    const auto result = boost::urls::parse_uri(value);
    if (!result) return std::string{"invalid WebSocket URL"};
    const auto url = *result;
    if (!beast::iequals(url.scheme(), "ws")) return std::string{"WebSocket URL scheme must be ws"};
    if (url.has_userinfo()) return std::string{"WebSocket URL cannot contain user information"};
    if (url.has_fragment()) return std::string{"WebSocket URL fragments are not supported"};
    if (url.host().empty()) return std::string{"WebSocket URL has no host"};

    const auto host = std::string{url.host()};
    if (std::ranges::any_of(host, [](unsigned char character) {
            return character == 0 || character < 0x20 || character == 0x7f;
        })) {
        return std::string{"WebSocket URL host contains a control character"};
    }
    if (url.host_type() == boost::urls::host_type::ipvfuture) {
        return std::string{"IPvFuture WebSocket hosts are not supported"};
    }
    parsed.host = url.host_type() == boost::urls::host_type::ipv6 ? std::string{url.host_address()} : host;
    parsed.host_header = std::string{url.encoded_authority()};
    parsed.service = "80";
    if (url.has_port()) {
        const auto port = url.port();
        auto port_value = std::uint32_t{};
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), port_value);
        if (port.empty() || error != std::errc{} || end != port.data() + port.size() || port_value == 0 ||
            port_value > 65535) {
            return std::string{"WebSocket URL port must be between 1 and 65535"};
        }
        parsed.service = std::string{port};
    }
    parsed.target = url.encoded_path().empty() ? "/" : std::string{url.encoded_path()};
    if (url.has_query()) parsed.target += '?' + std::string{url.encoded_query()};
    return std::nullopt;
}

bool websocket_managed_header(std::string_view name) {
    return beast::iequals(name, "host") || beast::iequals(name, "connection") || beast::iequals(name, "upgrade") ||
           beast::iequals(name, "content-length") || beast::iequals(name, "transfer-encoding") ||
           beast::iequals(name, "trailer") || beast::iequals(name, "sec-websocket-key") ||
           beast::iequals(name, "sec-websocket-version") || beast::iequals(name, "sec-websocket-accept") ||
           beast::iequals(name, "sec-websocket-extensions");
}

ScalarValue<Bundle<HttpResponse>> response_value(i64 status, HeaderValues headers,
                                                 std::span<const std::byte> body) {
    for (auto &[name, value] : headers) details::convert_latin1_header_value_as_utf8(value);
    auto memory = allocator{};
    return bundle<HttpResponse>(memory, "status"_n = status,
                                "headers"_n = std::span<const std::pair<std::string, std::string>>{headers},
                                "body"_n = body);
}

ScalarValue<Bundle<HttpResponse>> error_response(std::string_view message) {
    return response_value(599,
                          {},
                          std::span<const std::byte>{reinterpret_cast<const std::byte *>(message.data()),
                                                     message.size()});
}

ScalarValue<Bundle<WebSocketClose>> close_value(i64 code, std::string_view reason) {
    auto memory = allocator{};
    return bundle<WebSocketClose>(memory, "code"_n = code, "reason"_n = reason);
}

} // namespace

class WebSocketClientImplementation final {
  public:
    WebSocketClientImplementation(WebSocketClientOptions options, Queue output)
        : options_{std::move(options)}, output_{std::move(output)}, work_{asio::make_work_guard(io_)} {
        if (options_.handshake_timeout.count() <= 0) {
            throw std::invalid_argument{"WebSocket handshake timeout must be positive"};
        }
        if (options_.max_message_size == 0) {
            throw std::invalid_argument{"WebSocket maximum message size must be positive"};
        }
        thread_ = std::jthread{[this] {
            try {
                io_.run();
            } catch (...) {
                record_failure(std::current_exception());
            }
        }};
    }

    ~WebSocketClientImplementation() { stop(); }

    void attach_message_queue(Queue messages) {
        if (started_ || message_queue_attached_) {
            throw std::logic_error{"the WebSocket message queue must be attached once before the client starts"};
        }
        messages_ = std::move(messages);
        message_queue_attached_ = true;
    }

    void update(i64 client_id, i64 sequence,
                std::optional<ScalarValue<Bundle<WebSocketConnectRequest>>> connect,
                std::optional<ScalarValue<bytes>> message,
                std::optional<ScalarValue<Bundle<WebSocketClose>>> close) {
        if (stopped_.load(std::memory_order_acquire)) return;
        started_ = true;
        asio::post(io_, [this, client_id, sequence, connect = std::move(connect), message = std::move(message),
                         close = std::move(close)]() mutable {
            apply_update(client_id, sequence, std::move(connect), std::move(message), std::move(close));
        });
    }

    void cancel(i64 client_id, i64 sequence) noexcept {
        if (stopped_.load(std::memory_order_acquire)) return;
        try {
            asio::post(io_, [this, client_id, sequence] {
                const auto found = active_.find(client_id);
                if (found == active_.end() || found->second.sequence != sequence) return;
                auto connection = std::move(found->second.connection);
                active_.erase(found);
                clear_bucket(client_id, sequence, true);
                if (connection) connection->cancel();
            });
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void stop() noexcept {
        if (stopped_.exchange(true, std::memory_order_acq_rel)) {
            if (thread_.joinable()) thread_.join();
            return;
        }
        io_.stop();
        if (thread_.joinable()) thread_.join();
        for (auto &[client_id, active] : active_) {
            (void)client_id;
            if (active.connection) active.connection->cancel();
        }
        active_.clear();
    }

  private:
    class Connection final : public std::enable_shared_from_this<Connection> {
      public:
        Connection(WebSocketClientImplementation &client, i64 client_id, i64 sequence)
            : client_{client}, client_id_{client_id}, sequence_{sequence}, resolver_{client.io_},
              timeout_{client.io_}, stream_{client.io_} {}

        i64 sequence() const noexcept { return sequence_; }

        void start(ScalarValue<Bundle<WebSocketConnectRequest>> connect) {
            try {
                const auto value = connect.view();
                const auto url = std::string{scalar_cast<string>(value[0]).value()};
                if (const auto error = parse_url(url, parsed_url_)) {
                    fail_before_handshake(*error);
                    return;
                }

                const auto headers = scalar_cast<List<Header>>(value[1]);
                request_headers_.reserve(headers.size());
                for (auto index = std::size_t{}; index < headers.size(); ++index) {
                    const auto header = headers[index];
                    auto name = std::string{scalar_cast<string>(header.template get<0>()).value()};
                    const auto raw_value = std::string{scalar_cast<string>(header.template get<1>()).value()};
                    if (!details::valid_http_header_name(name)) {
                        fail_before_handshake("WebSocket header name is not a valid token");
                        return;
                    }
                    if (websocket_managed_header(name)) {
                        fail_before_handshake("WebSocket header controls handshake framing");
                        return;
                    }
                    const auto value = details::utf8_header_value_to_latin1(raw_value);
                    if (!value) {
                        fail_before_handshake("WebSocket header value must be Latin-1 text without line breaks");
                        return;
                    }
                    if (beast::iequals(name, "sec-websocket-protocol") && !append_protocols(*value)) {
                        fail_before_handshake("WebSocket subprotocol offer is invalid");
                        return;
                    }
                    request_headers_.emplace_back(std::move(name), *value);
                }

                auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
                timeout.handshake_timeout = client_.options_.handshake_timeout;
                stream_.set_option(timeout);
                stream_.read_message_max(client_.options_.max_message_size);
                stream_.set_option(websocket::stream_base::decorator(
                    [headers = request_headers_](websocket::request_type &request) {
                        for (const auto &[name, value] : headers) request.insert(name, value);
                    }));
                state_ = State::resolving;
                auto self = shared_from_this();
                timeout_.expires_after(client_.options_.handshake_timeout);
                timeout_.async_wait([self](beast::error_code error) {
                    if (error || !self->current() || self->state_ == State::finished || self->state_ == State::open ||
                        self->state_ == State::closing) {
                        return;
                    }
                    self->fail_before_handshake("WebSocket connection timed out");
                });
                resolver_.async_resolve(parsed_url_.host, parsed_url_.service,
                                        [self](beast::error_code error, tcp::resolver::results_type results) {
                                            self->on_resolve(error, std::move(results));
                                        });
            } catch (const std::exception &error) {
                fail_before_handshake(error.what());
            }
        }

        void enqueue_message(ScalarValue<bytes> message) {
            if (state_ == State::finished || state_ == State::closing) return;
            writes_.push_back(std::move(message));
            write_next();
        }

        void close(ScalarValue<Bundle<WebSocketClose>> close_value) {
            if (state_ == State::finished || state_ == State::closing) return;
            if (state_ != State::open) {
                fail_before_handshake("WebSocket handshake canceled by close request");
                return;
            }
            const auto value = close_value.view();
            const auto code = scalar_cast<i64>(value[0]).value();
            const auto reason = std::string{scalar_cast<string>(value[1]).value()};
            const auto valid_code = (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) ||
                                    (code >= 3000 && code <= 4999);
            if (reason.size() > 123 || !valid_utf8(reason)) {
                close_with_code(websocket::close_code::internal_error);
                return;
            }
            if (!valid_code) {
                close_with_code(websocket::close_code::internal_error);
                return;
            }
            auto close_reason = websocket::close_reason{};
            close_reason.code = static_cast<websocket::close_code>(code);
            close_reason.reason = reason;
            begin_close(std::move(close_reason));
        }

        void cancel() noexcept {
            if (state_ == State::finished) return;
            state_ = State::finished;
            resolver_.cancel();
            try {
                (void)timeout_.cancel();
            } catch (...) {
            }
            beast::error_code ignored;
            stream_.next_layer().cancel(ignored);
            stream_.next_layer().close(ignored);
        }

      private:
        enum class State { pending, resolving, connecting, handshaking, open, closing, finished };

        static bool valid_utf8(std::string_view value) noexcept {
            auto index = std::int32_t{};
            const auto length = static_cast<std::int32_t>(value.size());
            const auto *data = reinterpret_cast<const std::uint8_t *>(value.data());
            while (index < length) {
                auto code_point = UChar32{};
                U8_NEXT(data, index, length, code_point);
                if (code_point < 0) return false;
            }
            return true;
        }

        bool append_protocols(std::string_view value) {
            while (true) {
                const auto comma = value.find(',');
                auto token = value.substr(0, comma);
                while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
                while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
                if (!details::valid_http_header_name(token) ||
                    std::ranges::find(offered_protocols_, token) != offered_protocols_.end()) {
                    return false;
                }
                offered_protocols_.emplace_back(token);
                if (comma == std::string_view::npos) return true;
                value.remove_prefix(comma + 1);
            }
        }

        bool valid_subprotocol_response(const websocket::response_type &response) const {
            auto selected = false;
            for (const auto &field : response.base()) {
                if (!beast::iequals(field.name_string(), "sec-websocket-protocol")) continue;
                const auto value = std::string_view{field.value()};
                if (selected || value.find(',') != std::string_view::npos ||
                    !details::valid_http_header_name(value) ||
                    std::ranges::find(offered_protocols_, value) == offered_protocols_.end()) {
                    return false;
                }
                selected = true;
            }
            return !offered_protocols_.empty() || !selected;
        }

        void on_resolve(beast::error_code error, tcp::resolver::results_type results) {
            if (!current() || state_ == State::finished) return;
            if (error) {
                fail_before_handshake("WebSocket connection failed: " + error.message());
                return;
            }
            state_ = State::connecting;
            auto self = shared_from_this();
            asio::async_connect(stream_.next_layer(), results,
                                [self](beast::error_code connect_error, const tcp::endpoint &) {
                                    self->on_connect(connect_error);
                                });
        }

        void on_connect(beast::error_code error) {
            if (!current() || state_ == State::finished) return;
            if (error) {
                fail_before_handshake("WebSocket connection failed: " + error.message());
                return;
            }
            state_ = State::handshaking;
            auto self = shared_from_this();
            stream_.async_handshake(response_, parsed_url_.host_header, parsed_url_.target,
                                    [self](beast::error_code handshake_error) {
                                        self->on_handshake(handshake_error);
                                    });
        }

        void on_handshake(beast::error_code error) {
            if (!current() || state_ == State::finished) return;
            if (error) {
                if (error == websocket::error::upgrade_declined && response_.result_int() != 0 &&
                    response_.result() != http::status::switching_protocols) {
                    (void)timeout_.cancel();
                    write_connect_response(response_);
                    state_ = State::finished;
                    close_socket();
                    client_.finish_connection(client_id_, sequence_, this);
                    return;
                }
                fail_before_handshake("WebSocket handshake failed: " + error.message());
                return;
            }
            state_ = State::open;
            (void)timeout_.cancel();
            if (!valid_subprotocol_response(response_)) {
                client_.write_connect_response(client_id_, sequence_, error_response("Invalid WebSocket subprotocol"));
                close_with_code(websocket::close_code::protocol_error);
                return;
            }
            write_connect_response(response_);
            read_next();
            write_next();
        }

        void write_connect_response(const websocket::response_type &response) {
            auto headers = HeaderValues{};
            for (const auto &field : response.base()) {
                headers.emplace_back(std::string{field.name_string()}, std::string{field.value()});
            }
            const auto body = std::span<const std::byte>{
                reinterpret_cast<const std::byte *>(response.body().data()), response.body().size()};
            client_.write_connect_response(client_id_, sequence_,
                                           response_value(static_cast<i64>(response.result_int()), std::move(headers),
                                                          body));
        }

        void read_next() {
            if (state_ != State::open) return;
            auto self = shared_from_this();
            stream_.async_read(read_buffer_, [self](beast::error_code error, std::size_t) {
                self->on_read(error);
            });
        }

        void on_read(beast::error_code error) {
            if (!current() || state_ == State::finished) return;
            if (error == websocket::error::closed) {
                finish_from_remote();
                return;
            }
            if (error) {
                finish_abnormally();
                return;
            }
            if (!stream_.got_binary()) {
                close_with_code(websocket::close_code::unknown_data);
                return;
            }
            const auto data = read_buffer_.data();
            const auto payload = std::span<const std::byte>{static_cast<const std::byte *>(data.data()), data.size()};
            client_.write_message(client_id_, sequence_, payload);
            read_buffer_.consume(read_buffer_.size());
            read_next();
        }

        void write_next() {
            if ((state_ != State::open && state_ != State::closing) || write_started_) return;
            if (writes_.empty()) {
                if (state_ == State::closing) start_close();
                return;
            }
            write_started_ = true;
            const auto data = writes_.front().view().value();
            auto self = shared_from_this();
            stream_.binary(true);
            stream_.async_write(asio::buffer(data.data(), data.size()), [self](beast::error_code error, std::size_t) {
                self->write_started_ = false;
                if (error) {
                    self->finish_abnormally();
                    return;
                }
                self->writes_.pop_front();
                self->write_next();
            });
        }

        void close_with_code(websocket::close_code code) {
            auto reason = websocket::close_reason{};
            reason.code = code;
            begin_close(std::move(reason));
        }

        void begin_close(websocket::close_reason reason) {
            if (state_ != State::open) return;
            state_ = State::closing;
            close_reason_ = std::move(reason);
            write_next();
        }

        void start_close() {
            if (state_ != State::closing || close_started_) return;
            close_started_ = true;
            auto self = shared_from_this();
            stream_.async_close(close_reason_, [self](beast::error_code error) {
                if (error == websocket::error::closed) {
                    self->finish_from_remote();
                } else if (error) {
                    self->finish_abnormally();
                } else {
                    self->finish_from_remote();
                }
            });
        }

        void finish_from_remote() {
            const auto reason = stream_.reason();
            const auto code = static_cast<unsigned>(reason.code);
            const auto observed_code = code == 0 ? i64{1005} : static_cast<i64>(code);
            finish(observed_code, reason.reason);
        }

        void finish_abnormally() { finish(1006, {}); }

        void finish(i64 code, std::string_view reason) {
            if (state_ == State::finished) return;
            state_ = State::finished;
            client_.write_close(client_id_, sequence_, close_value(code, reason));
            close_socket();
            client_.finish_connection(client_id_, sequence_, this);
        }

        void fail_before_handshake(std::string_view message) {
            if (state_ == State::finished) return;
            state_ = State::finished;
            (void)timeout_.cancel();
            client_.write_connect_response(client_id_, sequence_, error_response(message));
            resolver_.cancel();
            close_socket();
            writes_.clear();
            client_.finish_connection(client_id_, sequence_, this);
        }

        void close_socket() noexcept {
            beast::error_code ignored;
            stream_.next_layer().cancel(ignored);
            stream_.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
            stream_.next_layer().close(ignored);
        }

        bool current() const noexcept { return client_.is_current(client_id_, sequence_, this); }

        WebSocketClientImplementation &client_;
        i64 client_id_;
        i64 sequence_;
        tcp::resolver resolver_;
        asio::steady_timer timeout_;
        websocket::stream<tcp::socket> stream_;
        beast::flat_buffer read_buffer_;
        websocket::response_type response_;
        ParsedUrl parsed_url_;
        HeaderValues request_headers_;
        std::deque<ScalarValue<bytes>> writes_;
        std::vector<std::string> offered_protocols_;
        websocket::close_reason close_reason_;
        State state_{State::pending};
        bool write_started_{};
        bool close_started_{};
    };

    void apply_update(i64 client_id, i64 sequence,
                      std::optional<ScalarValue<Bundle<WebSocketConnectRequest>>> connect,
                      std::optional<ScalarValue<bytes>> message,
                      std::optional<ScalarValue<Bundle<WebSocketClose>>> close) {
        if (connect) {
            const auto found = active_.find(client_id);
            if (found != active_.end()) {
                auto previous = std::move(found->second);
                active_.erase(found);
                clear_bucket(client_id, previous.sequence, false);
                if (previous.connection) previous.connection->cancel();
            }
            auto connection = std::make_shared<Connection>(*this, client_id, sequence);
            active_.emplace(client_id, ActiveConnection{.sequence = sequence, .connection = connection});
            connection->start(std::move(*connect));
            if (message) connection->enqueue_message(std::move(*message));
            if (close) connection->close(std::move(*close));
            return;
        }

        const auto found = active_.find(client_id);
        if (found == active_.end() || found->second.sequence != sequence || !found->second.connection) return;
        if (message) found->second.connection->enqueue_message(std::move(*message));
        if (close) found->second.connection->close(std::move(*close));
    }

    bool is_current(i64 client_id, i64 sequence, const Connection *connection) const noexcept {
        const auto found = active_.find(client_id);
        return found != active_.end() && found->second.sequence == sequence &&
               found->second.connection.get() == connection;
    }

    void finish_connection(i64 client_id, i64 sequence, const Connection *connection) noexcept {
        const auto found = active_.find(client_id);
        if (found == active_.end() || found->second.sequence != sequence ||
            found->second.connection.get() != connection) {
            return;
        }
        found->second.connection.reset();
    }

    void write_connect_response(i64 client_id, i64 sequence, ScalarValue<Bundle<HttpResponse>> response) {
        try {
            const auto accepted = output_.write(literal::TimeSeriesLiteral{
                {util::symbol{"connect_responses"},
                 {{client_id, {{sequence, response.view()}}}}}});
            if (!accepted && !stopped_.load(std::memory_order_acquire)) {
                record_failure(std::make_exception_ptr(std::runtime_error{"WebSocket client output queue is closed"}));
            }
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void write_close(i64 client_id, i64 sequence, ScalarValue<Bundle<WebSocketClose>> close) {
        try {
            const auto accepted = output_.write(literal::TimeSeriesLiteral{
                {util::symbol{"closes"}, {{client_id, {{sequence, close.view()}}}}}});
            if (!accepted && !stopped_.load(std::memory_order_acquire)) {
                record_failure(std::make_exception_ptr(std::runtime_error{"WebSocket client output queue is closed"}));
            }
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void write_message(i64 client_id, i64 sequence, std::span<const std::byte> message) {
        if (!message_queue_attached_) return;
        try {
            const auto accepted = messages_.write(literal::TimeSeriesLiteral{
                {client_id, {{sequence, literal::ScalarLiteral(message)}}}});
            if (!accepted && !stopped_.load(std::memory_order_acquire)) {
                record_failure(std::make_exception_ptr(std::runtime_error{"WebSocket message queue is closed"}));
            }
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void clear_bucket(i64 client_id, i64 sequence, bool remove_client) noexcept {
        try {
            if (message_queue_attached_) {
                if (remove_client) {
                    (void)messages_.write(literal::TimeSeriesLiteral{{client_id, literal::REMOVE}});
                } else {
                    (void)messages_.write(literal::TimeSeriesLiteral{{client_id, {{sequence, literal::REMOVE}}}});
                }
            }
            if (remove_client) {
                (void)output_.write(literal::TimeSeriesLiteral{
                    {util::symbol{"connect_responses"}, {{client_id, literal::REMOVE}}},
                    {util::symbol{"closes"}, {{client_id, literal::REMOVE}}}});
            } else {
                (void)output_.write(literal::TimeSeriesLiteral{
                    {util::symbol{"connect_responses"}, {{client_id, {{sequence, literal::REMOVE}}}}},
                    {util::symbol{"closes"}, {{client_id, {{sequence, literal::REMOVE}}}}}});
            }
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void record_failure(std::exception_ptr failure) noexcept {
        if (failure_reported_.exchange(true, std::memory_order_acq_rel)) return;
        try {
            auto message = std::string{"unexpected WebSocket client transport failure"};
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception &error) {
                message = error.what();
            } catch (...) {
            }
            (void)output_.write(literal::TimeSeriesLiteral{{util::symbol{"errors"}, message}});
        } catch (...) {
        }
    }

    WebSocketClientOptions options_;
    Queue output_;
    Queue messages_;
    asio::io_context io_{1};
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::jthread thread_;
    struct ActiveConnection {
        i64 sequence;
        std::shared_ptr<Connection> connection;
    };
    std::unordered_map<i64, ActiveConnection> active_;
    std::atomic<bool> stopped_{};
    std::atomic<bool> failure_reported_{};
    bool message_queue_attached_{};
    bool started_{};
};

WebSocketClientRuntime::WebSocketClientRuntime(WebSocketClientOptions options, Queue output)
    : implementation_{std::make_unique<WebSocketClientImplementation>(std::move(options), std::move(output))} {}

WebSocketClientRuntime::~WebSocketClientRuntime() = default;

i64 WebSocketClientRuntime::next_connection_sequence() {
    if (next_sequence_ == std::numeric_limits<i64>::max()) {
        throw std::overflow_error{"WebSocket connection sequence exhausted"};
    }
    return ++next_sequence_;
}

void WebSocketClientRuntime::attach_message_queue(Queue messages) {
    implementation_->attach_message_queue(std::move(messages));
}

void WebSocketClientRuntime::update(i64 client_id, i64 sequence,
                                    std::optional<ScalarValue<Bundle<WebSocketConnectRequest>>> connect,
                                    std::optional<ScalarValue<bytes>> message,
                                    std::optional<ScalarValue<Bundle<WebSocketClose>>> close) {
    implementation_->update(client_id, sequence, std::move(connect), std::move(message), std::move(close));
}

void WebSocketClientRuntime::cancel(i64 client_id, i64 sequence) noexcept {
    implementation_->cancel(client_id, sequence);
}

} // namespace bg::web
