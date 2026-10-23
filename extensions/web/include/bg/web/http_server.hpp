#pragma once

#include <bg/graph/wiring_template.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/web/export.hpp>
#include <bg/web/types.hpp>

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>

namespace bg::web {

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
};

struct Route {
    std::string_view path;
    const WiringTemplate &handler;
};

// Creates an HTTP/1.1 server and wires its request routes into the current graph.
// The returned time series ticks with the bound port after the listener is ready.
BG_WEB_EXPORT wp<TS<i64>> http_server(std::span<const Route> routes, ServerOptions options = {});

inline wp<TS<i64>> http_server(std::initializer_list<Route> routes, ServerOptions options = {}) {
    return http_server(std::span<const Route>{routes.begin(), routes.size()}, std::move(options));
}

} // namespace bg::web
