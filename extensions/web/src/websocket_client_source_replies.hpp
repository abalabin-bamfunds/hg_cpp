#pragma once

#include <bg/graph.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/web/types.hpp>
#include <bg/web/websocket_client.hpp>

#include "websocket_client_runtime.hpp"

namespace bg::web::details {

template <template <typename, auto...> typename Field> struct WebSocketClientSourceReplies {
    Field<TS<Opaque<WebSocketClientRuntime *>>> runtime;
    Field<TSD<service_client_id_t, TSB<BinaryWebSocketClientReply>>> replies;
};

wp<TSB<WebSocketClientSourceReplies>> wire_websocket_client_source_replies(WebSocketClientOptions options);

} // namespace bg::web::details
