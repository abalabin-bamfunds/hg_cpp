#include <bg/python/graph_definition.hpp>
#include <bg/python/schema.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/web/http_client.hpp>
#include <bg/web/http_server.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <cstdint>

namespace nb = nanobind;

namespace bg::web {
namespace {

template <template <template <typename, auto...> typename> typename SCHEMA>
void export_native_schema(nb::handle python_type) {
    const auto &time_series_schema = static_cast<const TimeSeriesBundleType &>(TSB<SCHEMA>::type()).schema();
    const auto &scalar_schema = details::hierarchical_scalar_type<SCHEMA>().schema();
    python::export_schema(python_type, time_series_schema, &scalar_schema);
}

python::PythonWiringPoint make_http_server(nb::dict route_handlers, std::string host, std::uint16_t port) {
    auto paths = std::vector<std::string>{};
    auto handlers = std::vector<const WiringTemplate *>{};
    paths.reserve(route_handlers.size());
    handlers.reserve(route_handlers.size());
    for (const auto &[path, handler] : route_handlers) {
        paths.push_back(nb::cast<std::string>(path));
        const auto &definition = nb::cast<const python::PythonGraphDefinition &>(
            nb::getattr(handler, "_native_definition"));
        handlers.push_back(&definition);
    }

    auto routes = std::vector<Route>{};
    routes.reserve(paths.size());
    for (auto index = std::size_t{}; index < paths.size(); ++index) {
        routes.push_back({paths[index], *handlers[index]});
    }
    return python::PythonWiringPoint{
        static_cast<WiringPoint>(http_server(routes, ServerOptions{.host = std::move(host), .port = port})),
        python::PythonWiringScope::current()};
}

python::PythonWiringPoint make_http_client_impl(const python::PythonWiringPoint &requests,
                                                std::int64_t timeout_ms,
                                                std::size_t max_response_body_size) {
    auto implementation = http_client_impl(HttpClientOptions{
        .timeout = std::chrono::milliseconds{timeout_ms},
        .max_response_body_size = max_response_body_size,
    });
    const auto result = (*implementation)(requests.get(WiringContext::current().wiring()));
    if (!result) throw std::logic_error{"HTTP client implementation must produce responses"};
    return python::PythonWiringPoint{*result, python::PythonWiringScope::current()};
}

} // namespace
} // namespace bg::web

NB_MODULE(_bg_web, module) {
    module.def("_register_schemas", [](nb::handle peer, nb::handle server_request, nb::handle client_request,
                                       nb::handle response) {
        bg::web::export_native_schema<bg::web::HttpPeer>(peer);
        bg::web::export_native_schema<bg::web::HttpServerRequest>(server_request);
        bg::web::export_native_schema<bg::web::HttpClientRequest>(client_request);
        bg::web::export_native_schema<bg::web::HttpResponse>(response);
    });
    module.def("http_server", &bg::web::make_http_server, nb::arg("routes"),
               nb::arg("host") = "127.0.0.1", nb::arg("port") = 0);
    module.def("_http_client_impl", &bg::web::make_http_client_impl, nb::arg("requests"), nb::arg("timeout_ms"),
               nb::arg("max_response_body_size"));
}
