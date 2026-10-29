#include "client_runtime.hpp"

#include "http_header.hpp"
#include "runtime_logging.hpp"

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
#include <chrono>
#include <cctype>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <chrono>
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
    Transfer(i64 client_id, i64 client_sequence, ScalarValue<Bundle<HttpClientRequest>> request_value)
        : client_id{client_id}, client_sequence{client_sequence}, request_value{std::move(request_value)} {}

    CURL *easy{};
    curl_slist *request_headers{};
    i64 client_id{};
    i64 client_sequence{};
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
    std::optional<std::string> configure(const HttpClientOptions &options);

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
    i64 client_sequence;
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
    for (const auto &header : request_headers_values) {
        const auto &name = header.first;
        if (!details::valid_http_header_name(name)) return std::string{"HTTP header name is not a valid token"};
        const auto normalized_name = lowercase(name);
        if (normalized_name == "host" || normalized_name == "connection" || normalized_name == "content-length" ||
            normalized_name == "transfer-encoding" || normalized_name == "upgrade" || normalized_name == "trailer" ||
            normalized_name == "te" || normalized_name == "keep-alive" || normalized_name == "proxy-connection" ||
            normalized_name == "expect") {
            return std::string{"HTTP header controls request routing or framing"};
        }
    }
    return std::nullopt;
}

ScalarValue<Bundle<HttpResponse>> response_value(i64 status, HeaderValues headers,
                                                 ScalarValue<bytes> body_value) {
    for (auto &[name, raw_value] : headers) {
        details::convert_latin1_header_value_as_utf8(raw_value);
    }

    auto memory = allocator{};
    return bundle<HttpResponse>(memory, "status"_n = status,
                                "headers"_n = std::span<const std::pair<std::string, std::string>>{headers},
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

std::optional<std::string> Transfer::configure(const HttpClientOptions &options) {
    max_response_body_size = options.max_response_body_size;
    for (const auto &[name, raw_value] : request_headers_values) {
        const auto value = details::utf8_header_value_to_latin1(raw_value);
        if (!value) return std::string{"HTTP header value must be Latin-1 text without line breaks"};
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
    return std::nullopt;
}

} // namespace

class ClientImplementation final {
  public:
    ClientImplementation(HttpClientOptions options, Queue output) : options_{std::move(options)}, output_{std::move(output)} {
        initialize_curl();
        if (options_.timeout.count() <= 0 || options_.timeout.count() > std::numeric_limits<long>::max()) {
            throw std::invalid_argument{"HTTP client timeout must be positive and fit libcurl's millisecond limit"};
        }
        multi_ = create_multi();
        thread_ = std::jthread{[this] { run(); }};
    }

    ClientImplementation(const ClientImplementation &) = delete;
    ClientImplementation &operator=(const ClientImplementation &) = delete;

    ~ClientImplementation() {
        stop();
    }

    void request(i64 client_id, i64 client_sequence, ScalarValue<Bundle<HttpClientRequest>> request) {
        enqueue(Command{.kind = Command::Kind::request,
                        .client_id = client_id,
                        .client_sequence = client_sequence,
                        .request = std::move(request)});
    }

    void cancel(i64 client_id, i64 client_sequence) noexcept {
        try {
            enqueue(Command{.kind = Command::Kind::cancel,
                            .client_id = client_id,
                            .client_sequence = client_sequence});
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void stop() noexcept {
        if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
            if (thread_.joinable()) thread_.join();
            return;
        }
        {
            const auto lock = std::lock_guard{commands_mutex_};
            if (multi_) {
                const auto result = curl_multi_wakeup(multi_.get());
                if (result != CURLM_OK) record_failure(curl_multi_strerror(result));
            }
        }
        commands_changed_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void record_failure(std::exception_ptr failure) noexcept {
        details::log_runtime_failure("HTTP client runtime failure", failure);
    }

    void record_failure(std::string_view message) noexcept {
        details::log_runtime_failure("HTTP client runtime failure", message);
    }

  private:
    using MultiHandle = std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)>;

    static MultiHandle create_multi() {
        MultiHandle multi{curl_multi_init(), curl_multi_cleanup};
        if (!multi) throw std::runtime_error{"libcurl multi initialization failed"};
        const auto result = curl_multi_setopt(multi.get(), CURLMOPT_MAXCONNECTS, 8L);
        if (result != CURLM_OK) {
            throw std::runtime_error{std::string{"libcurl connection cache setup failed: "} +
                                     curl_multi_strerror(result)};
        }
        return multi;
    }

    void enqueue(Command command) {
        auto wake_result = CURLM_OK;
        {
            const auto lock = std::lock_guard{commands_mutex_};
            if (stop_requested_.load(std::memory_order_acquire)) return;
            commands_.push_back(std::move(command));
            if (multi_) wake_result = curl_multi_wakeup(multi_.get());
        }
        commands_changed_.notify_all();
        if (wake_result != CURLM_OK)
            record_failure(curl_multi_strerror(wake_result));
    }

    void run() noexcept {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            try {
                if (!multi_) {
                    wait_for_commands(std::chrono::milliseconds{50}, true);
                    if (stop_requested_.load(std::memory_order_acquire)) break;
                    try {
                        auto replacement = create_multi();
                        const auto lock = std::lock_guard{commands_mutex_};
                        multi_ = std::move(replacement);
                    } catch (...) {
                        record_failure(std::current_exception());
                    }
                    continue;
                }
                process_commands();
                if (stop_requested_.load(std::memory_order_acquire)) break;
                if (!multi_) continue;
                auto running = 0;
                auto result = curl_multi_perform(multi_.get(), &running);
                if (result != CURLM_OK) {
                    recover_multi(curl_multi_strerror(result));
                    continue;
                }
                process_completions();
                if (stop_requested_.load(std::memory_order_acquire)) break;
                if (!multi_) continue;
                auto descriptors = 0;
                result = curl_multi_poll(multi_.get(), nullptr, 0, 1000, &descriptors);
                if (result != CURLM_OK) {
                    record_failure(curl_multi_strerror(result));
                    wait_for_commands(std::chrono::milliseconds{50}, false);
                }
            } catch (...) {
                record_failure(std::current_exception());
                wait_for_commands(std::chrono::milliseconds{50}, true);
            }
        }
        discard_transfers();
    }

    void wait_for_commands(std::chrono::milliseconds duration, bool stop_only) {
        auto lock = std::unique_lock{commands_mutex_};
        commands_changed_.wait_for(lock, duration, [&] {
            return stop_requested_.load(std::memory_order_acquire) || (!stop_only && !commands_.empty());
        });
    }

    void recover_multi(std::string_view message) noexcept {
        record_failure(message);
        for (const auto &[easy, transfer] : transfers_) {
            (void)easy;
            write_error(transfer->client_id, "HTTP transport engine failed");
        }
        discard_transfers();
        try {
            auto replacement = create_multi();
            const auto lock = std::lock_guard{commands_mutex_};
            multi_ = std::move(replacement);
        } catch (...) {
            record_failure(std::current_exception());
        }
        wait_for_commands(std::chrono::milliseconds{50}, true);
    }

    void process_commands() {
        auto pending = std::vector<Command>{};
        {
            const auto lock = std::lock_guard{commands_mutex_};
            pending.swap(commands_);
        }
        for (auto &command : pending) {
            const auto client_id = command.client_id;
            const auto is_request = command.kind == Command::Kind::request;
            if (is_request && !multi_) {
                record_failure("libcurl transport is recovering; request was not started");
                write_error(client_id, "HTTP transport is unavailable");
                continue;
            }
            try {
                if (is_request) {
                    start_request(std::move(command));
                } else {
                    cancel_requests(command.client_id, command.client_sequence);
                }
            } catch (...) {
                record_failure(std::current_exception());
                if (is_request) write_error(client_id, "HTTP request could not be started");
            }
        }
    }

    void start_request(Command command) {
        auto transfer = std::make_unique<Transfer>(command.client_id, command.client_sequence,
                                                   std::move(*command.request));
        transfer->read_request();
        if (const auto error = transfer->validate()) {
            record_failure(*error);
            write_error(command.client_id, *error);
            return;
        }
        transfer->easy = curl_easy_init();
        if (transfer->easy == nullptr) {
            record_failure("libcurl request initialization failed");
            write_error(command.client_id, "HTTP request could not be initialized");
            return;
        }
        if (const auto error = transfer->configure(options_)) {
            record_failure(*error);
            write_error(command.client_id, *error);
            return;
        }

        auto *easy = transfer->easy;
        const auto [position, inserted] = transfers_.emplace(easy, std::move(transfer));
        if (!inserted) throw std::logic_error{"duplicate libcurl request handle"};
        const auto result = curl_multi_add_handle(multi_.get(), easy);
        if (result != CURLM_OK) {
            transfers_.erase(position);
            const auto message = std::string{"libcurl could not start a request: "} + curl_multi_strerror(result);
            record_failure(message);
            write_error(command.client_id, message);
        }
    }

    void cancel_requests(i64 client_id, i64 client_sequence) {
        for (auto iterator = transfers_.begin(); iterator != transfers_.end();) {
            const auto &transfer = *iterator->second;
            if (transfer.client_id == client_id && transfer.client_sequence == client_sequence) {
                const auto result = curl_multi_remove_handle(multi_.get(), iterator->first);
                if (result != CURLM_OK) {
                    recover_multi(curl_multi_strerror(result));
                    break;
                }
                iterator = transfers_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        try {
            const auto accepted = output_.write(literal::TimeSeriesLiteral{
                {util::symbol{"replies"}, {{client_id, literal::REMOVE}}}});
            if (!accepted && !stop_requested_.load(std::memory_order_acquire)) {
                record_failure("HTTP response queue is closed");
            }
        } catch (...) {
            record_failure(std::current_exception());
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
            const auto client_id = found->second->client_id;
            const auto remove_result = curl_multi_remove_handle(multi_.get(), easy);
            if (remove_result != CURLM_OK) {
                recover_multi(curl_multi_strerror(remove_result));
                return;
            }
            auto transfer = std::move(found->second);
            transfers_.erase(found);

            try {
                if (transfer->callback_failed) {
                    record_failure("libcurl response callback failed");
                    write_error(client_id, "HTTP response could not be collected");
                    continue;
                }
                if (transfer->response_too_large) {
                    record_failure("HTTP response body exceeds configured limit");
                    write_error(client_id, "HTTP response body exceeds configured limit");
                    continue;
                }
                if (result != CURLE_OK) {
                    const auto description = result == CURLE_OPERATION_TIMEDOUT
                                                 ? std::string{"HTTP request timed out"}
                                                 : std::string{"HTTP request failed: "} + transfer->curl_error(result);
                    record_failure(description);
                    write_error(client_id, description);
                    continue;
                }

                auto status = long{};
                const auto info_result = curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
                if (info_result != CURLE_OK) {
                    const auto description = std::string{"libcurl response status lookup failed: "} +
                                             curl_easy_strerror(info_result);
                    record_failure(description);
                    write_error(client_id, description);
                    continue;
                }
                if (status == 101) {
                    write_error(client_id, "HTTP protocol upgrade is not supported");
                    continue;
                }
                auto response = response_value(static_cast<i64>(status), std::move(transfer->response_headers),
                                               std::move(transfer->response_body).finish());
                write_response(client_id, std::move(response));
            } catch (...) {
                record_failure(std::current_exception());
                write_error(client_id, "HTTP response could not be processed");
            }
        }
    }

    void write_response(i64 client_id, ScalarValue<Bundle<HttpResponse>> response) noexcept {
        try {
            const auto accepted = output_.write(literal::TimeSeriesLiteral{
                {util::symbol{"replies"}, {{client_id, response.view()}}}});
            if (!accepted && !stop_requested_.load(std::memory_order_acquire)) {
                record_failure("HTTP response queue is closed");
            }
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void write_error(i64 client_id, std::string_view message) noexcept {
        try {
            write_response(client_id, error_response(message));
        } catch (...) {
            record_failure(std::current_exception());
        }
    }

    void discard_transfers() noexcept {
        for (const auto &[easy, transfer] : transfers_) {
            if (multi_) {
                const auto result = curl_multi_remove_handle(multi_.get(), easy);
                if (result != CURLM_OK) record_failure(curl_multi_strerror(result));
            }
            (void)transfer;
        }
        auto previous = MultiHandle{nullptr, curl_multi_cleanup};
        {
            const auto lock = std::lock_guard{commands_mutex_};
            previous = std::move(multi_);
        }
        previous.reset();
        transfers_.clear();
    }

    HttpClientOptions options_;
    Queue output_;
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi_{nullptr, curl_multi_cleanup};
    std::jthread thread_;
    std::mutex commands_mutex_;
    std::condition_variable commands_changed_;
    std::vector<Command> commands_;
    std::unordered_map<CURL *, std::unique_ptr<Transfer>> transfers_;
    std::atomic<bool> stop_requested_{};
};

ClientRuntime::ClientRuntime(HttpClientOptions options, Queue output)
    : implementation_{std::make_unique<ClientImplementation>(std::move(options), std::move(output))} {}

ClientRuntime::~ClientRuntime() = default;

void ClientRuntime::request(i64 client_id, i64 client_sequence, ScalarValue<Bundle<HttpClientRequest>> value) {
    implementation_->request(client_id, client_sequence, std::move(value));
}

void ClientRuntime::cancel(i64 client_id, i64 client_sequence) noexcept {
    implementation_->cancel(client_id, client_sequence);
}

} // namespace bg::web
