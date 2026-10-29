#pragma once

#include <bg/graph/wiring_point.hpp>
#include <bg/web/http_server.hpp>

namespace bg::web::details {

BG_WEB_EXPORT WiringPoint wire_http_server(ServerOptions options, WiringPoint routes, WiringPoint responses);
BG_WEB_EXPORT WiringPoint wire_http_server(ServerOptions options, WiringPoint routes, WiringPoint responses,
                                           WiringPoint websocket_routes, WiringPoint websocket_responses);
BG_WEB_EXPORT WiringPoint http_authenticate_requests(WiringPoint requests, WiringPoint decisions);
BG_WEB_EXPORT WiringPoint http_merge_responses(WiringPoint responses, WiringPoint rejections);
BG_WEB_EXPORT WiringPoint http_response_field(WiringPoint result);
BG_WEB_EXPORT bool http_handler_input_is_dictionary(const WiringTemplate &handler);

} // namespace bg::web::details
