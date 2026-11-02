#pragma once

#include <hg/graph/wiring_template.hpp>
#include <hg/service.hpp>
#include <hg/web/export.hpp>
#include <hg/web/types.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace hg::web {

struct WebSocketClientOptions {
    std::chrono::milliseconds handshake_timeout{std::chrono::seconds{30}};
    std::size_t max_message_size{8 * 1024 * 1024};
    std::optional<std::string> ca_file;
};

extern HG_WEB_EXPORT const service<auto(TSB<BinaryWebSocketClientRequest>) -> TSB<BinaryWebSocketClientReply>>
    websocket_client;

// Builds a request/reply service implementation for binary ws:// and wss:// connections.
HG_WEB_EXPORT std::shared_ptr<const WiringTemplate> websocket_client_impl(WebSocketClientOptions options = {});

} // namespace hg::web
