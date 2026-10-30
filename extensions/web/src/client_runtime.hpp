#pragma once

#include <bg/queue.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/web/http_client.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

namespace bg::web {

class ClientImplementation;

class ClientRuntime {
  public:
    ClientRuntime(HttpClientOptions options, Queue output);
    ClientRuntime(const ClientRuntime &) = delete;
    ClientRuntime &operator=(const ClientRuntime &) = delete;
    ~ClientRuntime();

    void request(i64 client_id, i64 client_sequence, ScalarValue<Bundle<HttpClientRequest>> request,
                 std::optional<ScalarValue<Bundle<Auth>>> credentials = std::nullopt);
    void request(i64 client_id, i64 client_sequence, ScalarValue<Bundle<HttpClientRequest>> request,
                 std::optional<ScalarValue<Bundle<Auth>>> credentials,
                 std::chrono::steady_clock::time_point deadline);
    void fail(i64 client_id, i64 client_sequence, std::string_view message) noexcept;
    void cancel(i64 client_id, i64 client_sequence) noexcept;

  private:
    std::unique_ptr<ClientImplementation> implementation_;
};

} // namespace bg::web
