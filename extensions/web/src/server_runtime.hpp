#pragma once

#include <bg/queue.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/web/http_server.hpp>

#include <memory>
#include <string>
#include <vector>

namespace bg::web {

class ServerImplementation;

class ServerRuntime {
  public:
    ServerRuntime(ServerOptions options, std::vector<std::string> routes, Queue output);
    ServerRuntime(const ServerRuntime &) = delete;
    ServerRuntime &operator=(const ServerRuntime &) = delete;
    ~ServerRuntime();

    std::uint16_t port() const noexcept;
    void respond(i64 request_id, ScalarValue<Bundle<HttpResponse>> response);

  private:
    std::unique_ptr<ServerImplementation> implementation_;
};

} // namespace bg::web
