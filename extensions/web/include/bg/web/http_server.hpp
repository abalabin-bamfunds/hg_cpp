#pragma once

#include <bg/graph/wiring_template.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/web/export.hpp>
#include <bg/web/auth.hpp>
#include <bg/web/types.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace bg::web {

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
};

inline const auto http_handler_service =
    service<auto(TS<string, TimeSeriesGuarantee::final>,
                 TSD<i64, TS<Bundle<HttpResponse>>>) -> TSD<i64, TS<Bundle<HttpServerRequest>>>>{
        "http_handler", {"route", "responses"}};
inline const auto http_server_port_service = reference_service<auto() -> TS<i64>>{"http_server_port"};

// Register one HTTP/1.1 listener implementation in a graph's service catalogue.
BG_WEB_EXPORT void register_http_server(ServiceCatalogue &catalogue, ServerOptions options = {});

// Return the bound port after the listener has installed its initial handler routes.
BG_WEB_EXPORT wp<TS<i64>> http_server();

class BG_WEB_EXPORT HttpHandler final : public WiringTemplate {
  public:
    HttpHandler(std::string path, std::shared_ptr<const WiringTemplate> handler,
                const Service *policy = nullptr);

    const Signature &default_signature() const final { return signature_; }

    template <typename... Arguments>
        requires(bg::details::graph_code_argument<Arguments> && ...)
    wp<> operator()(Arguments &&...arguments) const & {
        const auto output = WiringTemplate::operator()(std::forward<Arguments>(arguments)...);
        if (!output) {
            throw std::logic_error{"an HTTP handler must return a time series"};
        }
        return wp<>{*output};
    }

    wp<> operator()(std::span<const WiringPoint> arguments) const & {
        const auto output = WiringTemplate::operator()(arguments);
        if (!output) {
            throw std::logic_error{"an HTTP handler must return a time series"};
        }
        return wp<>{*output};
    }

    template <typename... Arguments>
        requires(bg::details::graph_code_argument<Arguments> && ...)
    wp<> operator()(Arguments &&...) const && = delete;

    wp<> operator()(std::span<const WiringPoint>) const && = delete;

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument>) const final;

    std::string path_;
    std::shared_ptr<const WiringTemplate> handler_;
    const Service *policy_{};
    Signature signature_;
};

template <typename Handler>
    requires std::derived_from<std::remove_cvref_t<Handler>, WiringTemplate>
HttpHandler http_handler(std::string path, Handler &&handler) {
    using Definition = std::remove_cvref_t<Handler>;
    return HttpHandler{std::move(path), std::make_shared<Definition>(std::forward<Handler>(handler))};
}

template <typename Handler>
    requires std::derived_from<std::remove_cvref_t<Handler>, WiringTemplate>
HttpHandler http_handler(std::string path, Handler &&handler, const Service &policy) {
    using Definition = std::remove_cvref_t<Handler>;
    return HttpHandler{std::move(path), std::make_shared<Definition>(std::forward<Handler>(handler)), &policy};
}

} // namespace bg::web
