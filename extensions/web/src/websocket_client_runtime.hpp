#pragma once

#include <bg/queue.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/web/websocket_client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bg::web {

class WebSocketClientImplementation;

class WebSocketClientRuntime {
  public:
    WebSocketClientRuntime(WebSocketClientOptions options, Queue output);
    WebSocketClientRuntime(const WebSocketClientRuntime &) = delete;
    WebSocketClientRuntime &operator=(const WebSocketClientRuntime &) = delete;
    ~WebSocketClientRuntime();

    // Called only by the graph thread. The sequence remains unique while queued output can exist.
    i64 next_connection_sequence();
    void attach_message_queue(Queue messages);
    void update(i64 client_id, i64 sequence,
                std::optional<ScalarValue<Bundle<WebSocketConnectRequest>>> connect,
                std::optional<ScalarValue<bytes>> message,
                std::optional<ScalarValue<Bundle<WebSocketClose>>> close,
                std::optional<ScalarValue<Bundle<Auth>>> credentials = std::nullopt,
                std::chrono::steady_clock::time_point deadline = {});
    void begin_connect(i64 client_id, i64 sequence);
    void fail(i64 client_id, i64 sequence, std::string_view message) noexcept;
    void cancel(i64 client_id, i64 sequence) noexcept;

  private:
    std::unique_ptr<WebSocketClientImplementation> implementation_;
    i64 next_sequence_{};
};

} // namespace bg::web
