#pragma once

#include <bg/queue.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/web/http_client.hpp>

#include <memory>

namespace bg::web {

class ClientImplementation;

class ClientRuntime {
  public:
    ClientRuntime(HttpClientOptions options, Queue output);
    ClientRuntime(const ClientRuntime &) = delete;
    ClientRuntime &operator=(const ClientRuntime &) = delete;
    ~ClientRuntime();

    void request(i64 client_id, i64 client_sequence, ScalarValue<Bundle<HttpClientRequest>> request);
    void cancel(i64 client_id, i64 client_sequence) noexcept;

  private:
    std::unique_ptr<ClientImplementation> implementation_;
};

} // namespace bg::web
