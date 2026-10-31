#pragma once

#include <bg/queue.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/web/http_server.hpp>

#include <memory>
#include <optional>
#include <string>

namespace bg::web {

class ServerImplementation;

struct WebSocketResponseUpdate {
    std::optional<ScalarValue<Bundle<HttpResponse>>> connect_response;
    std::optional<ScalarValue<bytes>> message;
    std::optional<ScalarValue<Bundle<WebSocketClose>>> close;
};

class ServerRuntime {
  public:
    ServerRuntime(ServerOptions options, Queue output);
    ServerRuntime(const ServerRuntime &) = delete;
    ServerRuntime &operator=(const ServerRuntime &) = delete;
    ~ServerRuntime();

    std::uint16_t port() const noexcept;
    void attach_websocket_queue(Queue messages);
    void register_http_route(service_client_id_t client_id, std::string route);
    void unregister_http_route(service_client_id_t client_id);
    void register_websocket_route(service_client_id_t client_id, std::string route);
    void unregister_websocket_route(service_client_id_t client_id);
    void start();
    void respond(service_client_id_t client_id, i64 request_id, ScalarValue<Bundle<HttpResponse>> response);
    void respond_websocket(service_client_id_t client_id, i64 connection_id, WebSocketResponseUpdate update);

  private:
    std::unique_ptr<ServerImplementation> implementation_;
};

} // namespace bg::web
