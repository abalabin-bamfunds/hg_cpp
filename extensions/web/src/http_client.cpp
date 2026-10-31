#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/http_client.hpp>

#include "client_request_sink.hpp"
#include "client_source.hpp"

#include <utility>

namespace bg::web {
using namespace bg::literals;

const service<auto(TS<Bundle<HttpClientRequest>>) -> TS<Bundle<HttpResponse>>> http_client{"http_client",
                                                                                         {"request"}};

std::shared_ptr<const WiringTemplate> http_client_impl(HttpClientOptions options) {
    auto implementation = graph(
        "name"_n = "http_client_impl",
        [options = std::move(options)](
            wp<TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>, "request"_n> requests)
            -> wp<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>> {
            const auto source = details::client_source(options);

            const auto client = graph(
                "name"_n = "http_client_request",
                [options](wp<TS<service_client_id_t>, "key"_n> client_id,
                   wp<TS<Bundle<HttpClientRequest>>, "request"_n> request,
                   wp<TS<Opaque<ClientRuntime *>>, "runtime"_n> runtime) {
                    const auto client_sequence = service_client_id();
                    details::client_request_sink(
                        client_id, client_sequence, request, request,
                        operators::nothing[type<TS<string>>](), runtime,
                        operators::nothing[type<TS<Bundle<Auth>>>](), options);
                });

            map(client, requests, source->runtime);
            return wp<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>>{source->replies};
        });
    return std::make_shared<decltype(implementation)>(std::move(implementation));
}

} // namespace bg::web
