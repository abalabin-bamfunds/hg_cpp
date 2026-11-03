#pragma once

#include <hg/graph/wiring_argument.hpp>
#include <hg/graph/wiring_template.hpp>
#include <hg/service.hpp>
#include <hg/service_catalogue.hpp>
#include <hg/web/export.hpp>
#include <hg/web/auth.hpp>
#include <hg/web/types.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace hg::web {

struct ServerTlsOptions {
    std::string certificate_chain_file;
    std::string private_key_file;
};

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
    std::optional<ServerTlsOptions> tls;
};

inline const auto http_handler_service =
    service<auto(TS<string, TimeSeriesGuarantee::final>,
                 TSD<i64, TS<Bundle<HttpResponse>>>) -> TSD<i64, TS<Bundle<HttpServerRequest>>>>{
        "http_handler", {"route", "responses"}};
extern HG_WEB_EXPORT const service<
    auto(TS<string, TimeSeriesGuarantee::final>, TSD<i64, TSB<BinaryWebSocketResponse>>) ->
        TSD<i64, TSB<BinaryWebSocketServerRequest>>>
    websocket_handler_service;
inline const auto http_server_port_service = reference_service<auto() -> TS<i64>>{"http_server_port"};

// Register one shared HTTP/1.1 and WebSocket listener in a graph's service catalogue.
HG_WEB_EXPORT void register_http_server(ServiceCatalogue &catalogue, ServerOptions options = {});

// Return the bound port after the listener has installed its initial handler routes.
HG_WEB_EXPORT wp<TS<i64>> http_server();

namespace details {

HG_WEB_EXPORT void validate_http_handler(std::string_view path, const WiringTemplate &handler,
                                       const Service *policy);
HG_WEB_EXPORT wp<> wire_http_handler(std::string_view path, const WiringTemplate &handler,
                                   const Service *policy, std::span<const WiringArgument> arguments);
HG_WEB_EXPORT void validate_websocket_handler(std::string_view path, const WiringTemplate &handler,
                                              const Service *policy);
HG_WEB_EXPORT wp<> wire_websocket_handler(std::string_view path, const WiringTemplate &handler, const Service *policy,
                                          std::span<const WiringArgument> arguments);

template <typename Handler>
    requires std::derived_from<std::remove_cvref_t<Handler>, WiringTemplate>
auto make_http_handler(std::string path, Handler &&handler, const Service *policy) {
    validate_http_handler(path, handler, policy);

    return [path = std::move(path), handler = std::forward<Handler>(handler), policy](auto &&...arguments) -> wp<> {
        const auto wiring_arguments = hg::details::make_wiring_arguments(
            std::forward<decltype(arguments)>(arguments)...);
        return wire_http_handler(path, handler, policy, wiring_arguments);
    };
}

template <typename Handler>
    requires std::derived_from<std::remove_cvref_t<Handler>, WiringTemplate>
auto make_websocket_handler(std::string path, Handler &&handler, const Service *policy) {
    validate_websocket_handler(path, handler, policy);

    return [path = std::move(path), handler = std::forward<Handler>(handler), policy](auto &&...arguments) -> wp<> {
        const auto wiring_arguments = hg::details::make_wiring_arguments(
            std::forward<decltype(arguments)>(arguments)...);
        return wire_websocket_handler(path, handler, policy, wiring_arguments);
    };
}

} // namespace details

template <typename Handler>
    requires std::derived_from<std::remove_cvref_t<Handler>, WiringTemplate>
auto http_handler(std::string path, Handler &&handler) {
    return details::make_http_handler(std::move(path), std::forward<Handler>(handler), nullptr);
}

template <typename Handler>
    requires std::derived_from<std::remove_cvref_t<Handler>, WiringTemplate>
auto http_handler(std::string path, Handler &&handler, const Service &policy) {
    return details::make_http_handler(std::move(path), std::forward<Handler>(handler), &policy);
}

template <typename Handler>
    requires std::derived_from<std::remove_cvref_t<Handler>, WiringTemplate>
auto websocket_handler(std::string path, Handler &&handler) {
    return details::make_websocket_handler(std::move(path), std::forward<Handler>(handler), nullptr);
}

template <typename Handler>
    requires std::derived_from<std::remove_cvref_t<Handler>, WiringTemplate>
auto websocket_handler(std::string path, Handler &&handler, const Service &policy) {
    return details::make_websocket_handler(std::move(path), std::forward<Handler>(handler), &policy);
}

} // namespace hg::web
