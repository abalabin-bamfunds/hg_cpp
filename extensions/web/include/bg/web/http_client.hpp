#pragma once

#include <bg/graph/wiring_template.hpp>
#include <bg/service.hpp>
#include <bg/web/export.hpp>
#include <bg/web/types.hpp>

#include <chrono>
#include <cstddef>
#include <memory>

namespace bg::web {

struct HttpClientOptions {
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    std::size_t max_response_body_size{8 * 1024 * 1024};
};

extern BG_WEB_EXPORT const service<auto(TS<Bundle<HttpClientRequest>>) -> TS<Bundle<HttpResponse>>> http_client;

// Builds a request/reply service implementation that sends each valid request update over HTTP/1.1.
BG_WEB_EXPORT std::shared_ptr<const WiringTemplate> http_client_impl(HttpClientOptions options = {});

} // namespace bg::web
