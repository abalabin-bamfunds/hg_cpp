#pragma once

#include <bg/queue.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/web/http_server.hpp>

#include <memory>
#include <string>

namespace bg::web {

class ServerImplementation;

class ServerRuntime {
  public:
    ServerRuntime(ServerOptions options, Queue output);
    ServerRuntime(const ServerRuntime &) = delete;
    ServerRuntime &operator=(const ServerRuntime &) = delete;
    ~ServerRuntime();

    std::uint16_t port() const noexcept;
    void register_route(service_client_id_t client_id, std::string route);
    void unregister_route(service_client_id_t client_id);
    void start();
    void respond(service_client_id_t client_id, i64 request_id, ScalarValue<Bundle<HttpResponse>> response);

  private:
    std::unique_ptr<ServerImplementation> implementation_;
};

} // namespace bg::web
