#pragma once

#include <hg/graph/wiring_point.hpp>
#include <hg/web/http_server.hpp>

namespace hg::web::details {

HG_WEB_EXPORT WiringPoint wire_http_server(ServerOptions options, WiringPoint routes, WiringPoint responses);
HG_WEB_EXPORT WiringPoint wire_http_server(ServerOptions options, WiringPoint routes, WiringPoint responses,
                                           WiringPoint websocket_routes, WiringPoint websocket_responses);
HG_WEB_EXPORT WiringPoint http_authenticate_requests(WiringPoint requests, WiringPoint decisions);
HG_WEB_EXPORT WiringPoint http_merge_responses(WiringPoint responses, WiringPoint rejections);
HG_WEB_EXPORT WiringPoint http_response_field(WiringPoint result);
HG_WEB_EXPORT bool http_handler_input_is_dictionary(const WiringTemplate &handler);

} // namespace hg::web::details
