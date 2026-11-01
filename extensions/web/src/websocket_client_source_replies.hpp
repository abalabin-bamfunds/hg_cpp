#pragma once

#include <hg/graph.hpp>
#include <hg/scalar/opaque.hpp>
#include <hg/time_series/tsb/tsb_type.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/web/types.hpp>
#include <hg/web/websocket_client.hpp>

#include "websocket_client_runtime.hpp"

namespace hg::web::details {

template <template <typename, auto...> typename Field> struct WebSocketClientSourceReplies {
    Field<TS<Opaque<WebSocketClientRuntime *>>> runtime;
    Field<TSD<service_client_id_t, TSB<BinaryWebSocketClientReply>>> replies;
};

wp<TSB<WebSocketClientSourceReplies>> wire_websocket_client_source_replies(WebSocketClientOptions options);

} // namespace hg::web::details
