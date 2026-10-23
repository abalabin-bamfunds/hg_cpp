#include "client_runtime.hpp"

#include "client_output.hpp"
#include "http_header.hpp"

#include <bg/literal/scalar_literal.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/bytes_builder.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
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

using HeaderPair = Tuple<string, string>;
using HeaderValues = std::vector<std::pair<std::string, std::string>>;
using namespace bg::literals;

struct Transfer {
    Transfer(i64 client_id, i64 generation, ScalarValue<Bundle<HttpClientRequest>> request_value)
        : client_id{client_id}, generation{generation}, request_value{std::move(request_value)} {}

    CURL *easy{};
    curl_slist *request_headers{};
    i64 client_id{};
    i64 generation{};
    ScalarValue<Bundle<HttpClientRequest>> request_value;
    std::string method;
    std::string url;
    HeaderValues request_headers_values;
    HeaderValues response_headers;
    BytesBuilder response_body;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    std::size_t max_response_body_size{};
    bool response_too_large{};
    bool callback_failed{};

    std::optional<std::string> validate() const;
    void configure(const HttpClientOptions &options);

    void read_request() {
        const auto request = request_value.view();
        method = std::string{scalar_cast<string>(request[0]).value()};
        url = std::string{scalar_cast<string>(request[1]).value()};
        const auto headers = scalar_cast<List<HeaderPair>>(request[2]);
        request_headers_values.reserve(headers.size());
        for (auto index = std::size_t{}; index < headers.size(); ++index) {
            const auto header = headers[index];
            request_headers_values.emplace_back(
                std::string{scalar_cast<string>(header.template get<0>()).value()},
                std::string{scalar_cast<string>(header.template get<1>()).value()});
        }
    }

    std::string curl_error(CURLcode code) const {
        if (error_buffer.front() != '\0') return error_buffer.data();
        return curl_easy_strerror(code);
    }

    static size_t receive_body(char *data, size_t size, size_t count, void *context) noexcept {
        auto &transfer = *static_cast<Transfer *>(context);
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            transfer.callback_failed = true;
            return 0;
        }
        const auto length = size * count;
        const auto body_size = transfer.response_body.view().size();
        if (body_size > transfer.max_response_body_size ||
            length > transfer.max_response_body_size - body_size) {
            transfer.response_too_large = true;
            return 0;
        }
        try {
            transfer.response_body.append(
                std::span<const std::byte>{reinterpret_cast<const std::byte *>(data), length});
        } catch (...) {
            transfer.callback_failed = true;
            return 0;
        }
        return length;
    }

    static size_t receive_headers(char *data, size_t size, size_t count, void *context) noexcept {
        auto &transfer = *static_cast<Transfer *>(context);
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            transfer.callback_failed = true;
            return 0;
        }
        const auto length = size * count;
        try {
            auto line = std::string_view{data, length};
            if (line.starts_with("HTTP/")) {
                transfer.response_headers.clear();
                return length;
            }
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
            if (line.empty()) return length;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) return length;
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
            transfer.response_headers.emplace_back(std::string{line.substr(0, colon)}, std::string{value});
        } catch (...) {
            transfer.callback_failed = true;
            return 0;
        }
        return length;
    }

    ~Transfer() {
        if (easy != nullptr) curl_easy_cleanup(easy);
        if (request_headers != nullptr) curl_slist_free_all(request_headers);
    }
};

struct Command {
    enum class Kind { request, cancel } kind;
    i64 client_id;
    i64 generation;
    std::optional<ScalarValue<Bundle<HttpClientRequest>>> request;
};

struct CurlGlobal {
    CurlGlobal() {
        const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error{"libcurl global initialization failed"};
        }
    }

    ~CurlGlobal() { curl_global_cleanup(); }
};

void initialize_curl() {
    static const CurlGlobal initialized;
    (void)initialized;
}

std::optional<std::string> validate_url(std::string_view value) {
    if (value.find('\0') != std::string_view::npos) return std::string{"HTTP URL contains a NUL byte"};
    auto *url = curl_url();
    if (url == nullptr) throw std::bad_alloc{};
    const auto cleanup = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>{url, curl_url_cleanup};
    const auto url_text = std::string{value};
    auto result = curl_url_set(url, CURLUPART_URL, url_text.c_str(), CURLU_DISALLOW_USER);
    if (result != CURLUE_OK) {
        return std::string{"invalid HTTP URL: "} + curl_url_strerror(result);
    }

    auto *scheme_value = static_cast<char *>(nullptr);
    result = curl_url_get(url, CURLUPART_SCHEME, &scheme_value, 0);
    const auto scheme = std::unique_ptr<char, decltype(&curl_free)>{scheme_value, curl_free};
    if (result != CURLUE_OK) return std::string{"HTTP URL has no scheme"};
    auto normalized_scheme = std::string{scheme.get()};
    std::ranges::transform(normalized_scheme, normalized_scheme.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized_scheme != "http" && normalized_scheme != "https") {
        return std::string{"URL scheme must be http or https"};
    }

    auto *host_value = static_cast<char *>(nullptr);
    result = curl_url_get(url, CURLUPART_HOST, &host_value, 0);
    const auto host = std::unique_ptr<char, decltype(&curl_free)>{host_value, curl_free};
    if (result != CURLUE_OK || host == nullptr || host.get()[0] == '\0') {
        return std::string{"HTTP URL has no host"};
    }

    auto *fragment_value = static_cast<char *>(nullptr);
    result = curl_url_get(url, CURLUPART_FRAGMENT, &fragment_value, 0);
    const auto fragment = std::unique_ptr<char, decltype(&curl_free)>{fragment_value, curl_free};
    if (result == CURLUE_OK && fragment != nullptr) return std::string{"URL fragments are not supported"};
    return std::nullopt;
}

std::string lowercase(std::string_view value) {
    auto result = std::string{value};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::optional<std::string> Transfer::validate() const {
    if (!details::valid_http_header_name(method)) return std::string{"HTTP method is not a valid token"};
    if (const auto url_error = validate_url(url)) return url_error;
    for (const auto &[name, value] : request_headers_values) {
        if (!details::valid_http_header_name(name)) return std::string{"HTTP header name is not a valid token"};
        const auto normalized_name = lowercase(name);
        if (normalized_name == "host" || normalized_name == "connection" || normalized_name == "content-length" ||
            normalized_name == "transfer-encoding" || normalized_name == "upgrade" || normalized_name == "trailer" ||
            normalized_name == "te" || normalized_name == "keep-alive" || normalized_name == "proxy-connection" ||
            normalized_name == "expect") {
            return std::string{"HTTP header controls request routing or framing"};
        }
        if (!details::utf8_header_value_to_latin1(value)) {
            return std::string{"HTTP header value must be Latin-1 text without line breaks"};
        }
    }
    return std::nullopt;
}

ScalarValue<List<HeaderPair>> header_values(const HeaderValues &headers) {
    auto values = std::vector<ScalarValue<HeaderPair>>{};
    auto views = std::vector<Scalar<HeaderPair>>{};
    values.reserve(headers.size());
    views.reserve(headers.size());
    for (const auto &[name, raw_value] : headers) {
        auto value = std::string{};
        details::append_latin1_header_value_as_utf8(value, raw_value);
        values.push_back(ScalarValue<HeaderPair>::from_literal(literal::ScalarLiteral{name, value}));
        views.push_back(values.back().view());
    }
    return ScalarValue<List<HeaderPair>>{views};
}

ScalarValue<Bundle<HttpResponse>> response_value(i64 status, const HeaderValues &headers,
                                                 ScalarValue<bytes> body_value) {
    auto header_list = header_values(headers);
    auto status_value = ScalarValue<i64>{status};
    auto memory = allocator{};
    return bundle<HttpResponse>(memory, "status"_n = status_value.view(), "headers"_n = header_list.view(),
                                "body"_n = body_value.view());
}

ScalarValue<Bundle<HttpResponse>> error_response(std::string_view message) {
    const auto body = std::span<const std::byte>{reinterpret_cast<const std::byte *>(message.data()), message.size()};
    return response_value(599, {}, ScalarValue<bytes>{body});
}

template <typename Value> void set_easy_option(CURL *easy, CURLoption option, Value value) {
    const auto result = curl_easy_setopt(easy, option, value);
    if (result != CURLE_OK) {
        throw std::runtime_error{std::string{"libcurl option failed: "} + curl_easy_strerror(result)};
    }
}

void Transfer::configure(const HttpClientOptions &options) {
    max_response_body_size = options.max_response_body_size;
    for (const auto &[name, raw_value] : request_headers_values) {
        const auto value = details::utf8_header_value_to_latin1(raw_value);
        if (!value) throw std::logic_error{"request header validation changed before transport"};
        auto field = name + (value->empty() ? ";" : ": " + *value);
        auto *updated = curl_slist_append(request_headers, field.c_str());
        if (updated == nullptr) throw std::bad_alloc{};
        request_headers = updated;
    }
    if (std::ranges::none_of(request_headers_values, [](const auto &header) {
            return lowercase(header.first) == "content-type";
        })) {
        auto *updated = curl_slist_append(request_headers, "Content-Type:");
        if (updated == nullptr) throw std::bad_alloc{};
        request_headers = updated;
    }
    auto *updated = curl_slist_append(request_headers, "Expect:");
    if (updated == nullptr) throw std::bad_alloc{};
    request_headers = updated;

    static constexpr char empty_body[] = "";
    set_easy_option(easy, CURLOPT_URL, url.c_str());
    set_easy_option(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    set_easy_option(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set_easy_option(easy, CURLOPT_FOLLOWLOCATION, 0L);
    set_easy_option(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    set_easy_option(easy, CURLOPT_NOSIGNAL, 1L);
    set_easy_option(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(options.timeout.count()));
    set_easy_option(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    set_easy_option(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    set_easy_option(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    set_easy_option(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
    set_easy_option(easy, CURLOPT_HTTPHEADER, request_headers);
    set_easy_option(easy, CURLOPT_WRITEFUNCTION, &Transfer::receive_body);
    set_easy_option(easy, CURLOPT_WRITEDATA, this);
    set_easy_option(easy, CURLOPT_HEADERFUNCTION, &Transfer::receive_headers);
    set_easy_option(easy, CURLOPT_HEADERDATA, this);
    set_easy_option(easy, CURLOPT_ERRORBUFFER, error_buffer.data());
    const auto body = scalar_cast<bytes>(request_value.view()[3]).value();
    if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
        const auto *body_data = body.empty() ? empty_body : reinterpret_cast<const char *>(body.data());
        set_easy_option(easy, CURLOPT_POSTFIELDS, body_data);
        set_easy_option(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    if (method == "HEAD") set_easy_option(easy, CURLOPT_NOBODY, 1L);
}

} // namespace

class ClientImplementation final {
  public:
    ClientImplementation(HttpClientOptions options, Queue output) : options_{std::move(options)}, output_{std::move(output)} {
        initialize_curl();
        if (options_.timeout.count() <= 0 || options_.timeout.count() > std::numeric_limits<long>::max()) {
            throw std::invalid_argument{"HTTP client timeout must be positive and fit libcurl's millisecond limit"};
        }
        multi_.reset(curl_multi_init());
        if (!multi_) throw std::runtime_error{"libcurl multi initialization failed"};
        const auto set_result = curl_multi_setopt(multi_.get(), CURLMOPT_MAXCONNECTS, 8L);
        if (set_result != CURLM_OK) {
            throw std::runtime_error{std::string{"libcurl connection cache setup failed: "} +
                                     curl_multi_strerror(set_result)};
        }
        thread_ = std::jthread{[this] { run(); }};
    }

    ClientImplementation(const ClientImplementation &) = delete;
    ClientImplementation &operator=(const ClientImplementation &) = delete;

    ~ClientImplementation() {
        stop();
    }

    void request(i64 client_id, i64 generation, ScalarValue<Bundle<HttpClientRequest>> request) {
        enqueue(Command{.kind = Command::Kind::request,
                        .client_id = client_id,
                        .generation = generation,
                        .request = std::move(request)});
    }

    void cancel(i64 client_id, i64 generation) noexcept {
        try {
            enqueue(Command{.kind = Command::Kind::cancel, .client_id = client_id, .generation = generation});
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void stop() noexcept {
        if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
            if (thread_.joinable()) thread_.join();
            return;
        }
        if (multi_) (void)curl_multi_wakeup(multi_.get());
        if (thread_.joinable()) thread_.join();
    }

    void record_failure(std::exception_ptr failure) noexcept {
        if (failure_reported_.exchange(true, std::memory_order_acq_rel)) return;
        try {
            auto message = std::string{"unexpected HTTP client transport failure"};
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

  private:
    void enqueue(Command command) {
        if (stop_requested_.load(std::memory_order_acquire)) return;
        {
            const auto lock = std::lock_guard{commands_mutex_};
            commands_.push_back(std::move(command));
        }
        const auto result = curl_multi_wakeup(multi_.get());
        if (result != CURLM_OK) {
            record_failure(std::make_exception_ptr(
                std::runtime_error{std::string{"libcurl transport wakeup failed: "} + curl_multi_strerror(result)}));
        }
    }

    void run() noexcept {
        try {
            while (!stop_requested_.load(std::memory_order_acquire)) {
                process_commands();
                if (stop_requested_.load(std::memory_order_acquire)) break;
                auto running = 0;
                auto result = curl_multi_perform(multi_.get(), &running);
                if (result != CURLM_OK) {
                    record_failure(std::make_exception_ptr(
                        std::runtime_error{std::string{"libcurl transfer engine failed: "} + curl_multi_strerror(result)}));
                    break;
                }
                process_completions();
                if (stop_requested_.load(std::memory_order_acquire)) break;
                auto descriptors = 0;
                result = curl_multi_poll(multi_.get(), nullptr, 0, 1000, &descriptors);
                if (result != CURLM_OK) {
                    record_failure(std::make_exception_ptr(
                        std::runtime_error{std::string{"libcurl poll failed: "} + curl_multi_strerror(result)}));
                    break;
                }
            }
        } catch (...) {
            record_failure(std::current_exception());
        }
        discard_transfers();
    }

    void process_commands() {
        auto pending = std::vector<Command>{};
        {
            const auto lock = std::lock_guard{commands_mutex_};
            pending.swap(commands_);
        }
        for (auto &command : pending) {
            if (command.kind == Command::Kind::request) {
                start_request(std::move(command));
            } else {
                cancel_requests(command.client_id, command.generation);
            }
        }
    }

    void start_request(Command command) {
        auto transfer = std::make_unique<Transfer>(command.client_id, command.generation,
                                                   std::move(*command.request));
        transfer->read_request();
        if (const auto error = transfer->validate()) {
            write_response(command.client_id, command.generation, error_response(*error));
            return;
        }
        transfer->easy = curl_easy_init();
        if (transfer->easy == nullptr) throw std::runtime_error{"libcurl request initialization failed"};
        transfer->configure(options_);

        auto *easy = transfer->easy;
        const auto [position, inserted] = transfers_.emplace(easy, std::move(transfer));
        if (!inserted) throw std::logic_error{"duplicate libcurl request handle"};
        const auto result = curl_multi_add_handle(multi_.get(), easy);
        if (result != CURLM_OK) {
            transfers_.erase(position);
            throw std::runtime_error{std::string{"libcurl could not start a request: "} + curl_multi_strerror(result)};
        }
    }

    void cancel_requests(i64 client_id, i64 generation) {
        for (auto iterator = transfers_.begin(); iterator != transfers_.end();) {
            const auto &transfer = *iterator->second;
            if (transfer.client_id == client_id && transfer.generation == generation) {
                (void)curl_multi_remove_handle(multi_.get(), iterator->first);
                iterator = transfers_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        const auto accepted = output_.write(literal::TimeSeriesLiteral{
            {util::symbol{"replies"}, {{client_id, literal::REMOVE}}}});
        if (!accepted && !stop_requested_.load(std::memory_order_acquire)) {
            record_failure(std::make_exception_ptr(std::runtime_error{"HTTP response queue is closed"}));
        }
    }

    void process_completions() {
        auto *message = static_cast<CURLMsg *>(nullptr);
        auto messages_left = 0;
        while ((message = curl_multi_info_read(multi_.get(), &messages_left)) != nullptr) {
            if (message->msg != CURLMSG_DONE) continue;
            const auto easy = message->easy_handle;
            const auto result = message->data.result;
            const auto found = transfers_.find(easy);
            if (found == transfers_.end()) continue;
            auto transfer = std::move(found->second);
            transfers_.erase(found);
            (void)curl_multi_remove_handle(multi_.get(), easy);

            if (transfer->callback_failed) {
                record_failure(std::make_exception_ptr(std::runtime_error{"libcurl response callback failed"}));
                continue;
            }
            if (transfer->response_too_large) {
                write_response(transfer->client_id, transfer->generation,
                               error_response("HTTP response body exceeds configured limit"));
                continue;
            }
            if (result != CURLE_OK) {
                const auto description = result == CURLE_OPERATION_TIMEDOUT
                                             ? std::string{"HTTP request timed out"}
                                             : std::string{"HTTP request failed: "} +
                                                   transfer->curl_error(result);
                write_response(transfer->client_id, transfer->generation, error_response(description));
                continue;
            }

            auto status = long{};
            const auto info_result = curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
            if (info_result != CURLE_OK) {
                throw std::runtime_error{std::string{"libcurl response status lookup failed: "} +
                                         curl_easy_strerror(info_result)};
            }
            if (status == 101) {
                write_response(transfer->client_id, transfer->generation,
                               error_response("HTTP protocol upgrade is not supported"));
                continue;
            }
            auto response = response_value(static_cast<i64>(status), transfer->response_headers,
                                           std::move(transfer->response_body).finish());
            write_response(transfer->client_id, transfer->generation, std::move(response));
        }
    }

    void write_response(i64 client_id, i64 generation, ScalarValue<Bundle<HttpResponse>> response) {
        auto memory = allocator{};
        auto generation_value = ScalarValue<i64>{generation};
        auto reply = bundle<details::ClientReply>(memory, "generation"_n = generation_value.view(),
                                                  "response"_n = response.view());
        const auto accepted = output_.write(literal::TimeSeriesLiteral{
            {util::symbol{"replies"}, {{client_id, reply.view()}}}});
        if (!accepted && !stop_requested_.load(std::memory_order_acquire)) {
            record_failure(std::make_exception_ptr(std::runtime_error{"HTTP response queue is closed"}));
        }
    }

    void discard_transfers() noexcept {
        for (auto &[easy, transfer] : transfers_) {
            (void)curl_multi_remove_handle(multi_.get(), easy);
            (void)transfer;
        }
        transfers_.clear();
    }

    HttpClientOptions options_;
    Queue output_;
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi_{nullptr, curl_multi_cleanup};
    std::jthread thread_;
    std::mutex commands_mutex_;
    std::vector<Command> commands_;
    std::unordered_map<CURL *, std::unique_ptr<Transfer>> transfers_;
    std::atomic<bool> stop_requested_{};
    std::atomic<bool> failure_reported_{};
};

ClientRuntime::ClientRuntime(HttpClientOptions options, Queue output)
    : implementation_{std::make_unique<ClientImplementation>(std::move(options), std::move(output))} {}

ClientRuntime::~ClientRuntime() = default;

void ClientRuntime::request(i64 client_id, i64 generation, ScalarValue<Bundle<HttpClientRequest>> value) {
    implementation_->request(client_id, generation, std::move(value));
}

void ClientRuntime::cancel(i64 client_id, i64 generation) noexcept {
    implementation_->cancel(client_id, generation);
}

} // namespace bg::web
