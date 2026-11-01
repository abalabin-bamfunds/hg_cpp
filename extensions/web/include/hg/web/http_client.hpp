#pragma once

#include <hg/graph/wiring_template.hpp>
#include <hg/service.hpp>
#include <hg/web/export.hpp>
#include <hg/web/types.hpp>

#include <chrono>
#include <cstddef>
#include <memory>

namespace hg::web {

struct HttpClientOptions {
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    std::size_t max_response_body_size{8 * 1024 * 1024};
};

extern HG_WEB_EXPORT const service<auto(TS<Bundle<HttpClientRequest>>) -> TS<Bundle<HttpResponse>>> http_client;

// Builds a request/reply service implementation that sends each valid request update over HTTP/1.1.
HG_WEB_EXPORT std::shared_ptr<const WiringTemplate> http_client_impl(HttpClientOptions options = {});

} // namespace hg::web
