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

#include "http_server_internal.hpp"

namespace nb = nanobind;

namespace bg::web {
namespace {

template <template <template <typename, auto...> typename> typename SCHEMA>
void export_native_schema(nb::handle python_type) {
    const auto &time_series_schema = static_cast<const TimeSeriesBundleType &>(TSB<SCHEMA>::type()).schema();
    const auto &scalar_schema = bg::details::hierarchical_scalar_type<SCHEMA>().schema();
    python::export_schema(python_type, time_series_schema, &scalar_schema);
}

python::PythonWiringPoint make_http_server_implementation(const python::PythonWiringPoint &routes,
                                                           const python::PythonWiringPoint &responses,
                                                           std::string host, std::uint16_t port) {
    auto &wiring = WiringContext::current().wiring();
    const auto output = details::wire_http_server(ServerOptions{.host = std::move(host), .port = port},
                                                  routes.get(wiring), responses.get(wiring));
    return python::PythonWiringPoint{output, python::PythonWiringScope::current()};
}

python::PythonWiringPoint make_http_response_field(const python::PythonWiringPoint &result) {
    auto &wiring = WiringContext::current().wiring();
    const auto output = details::http_response_field(result.get(wiring));
    return python::PythonWiringPoint{output, python::PythonWiringScope::current()};
}

python::PythonWiringPoint make_http_authenticate_requests(const python::PythonWiringPoint &requests,
                                                           const python::PythonWiringPoint &decisions) {
    auto &wiring = WiringContext::current().wiring();
    const auto output = details::http_authenticate_requests(requests.get(wiring), decisions.get(wiring));
    return python::PythonWiringPoint{output, python::PythonWiringScope::current()};
}

python::PythonWiringPoint make_http_merge_responses(const python::PythonWiringPoint &responses,
                                                    const python::PythonWiringPoint &rejections) {
    auto &wiring = WiringContext::current().wiring();
    const auto output = details::http_merge_responses(responses.get(wiring), rejections.get(wiring));
    return python::PythonWiringPoint{output, python::PythonWiringScope::current()};
}

python::PythonWiringPoint make_http_client_impl(const python::PythonWiringPoint &requests,
                                                std::int64_t timeout_ms,
                                                std::size_t max_response_body_size) {
    auto implementation = http_client_impl(HttpClientOptions{
        .timeout = std::chrono::milliseconds{timeout_ms},
        .max_response_body_size = max_response_body_size,
    });
    const auto result = (*implementation)(requests.get(WiringContext::current().wiring()));
    return python::PythonWiringPoint{*result, python::PythonWiringScope::current()};
}

} // namespace
} // namespace bg::web

NB_MODULE(_bg_web, module) {
    module.def("_register_schemas", [](nb::handle peer, nb::handle server_request, nb::handle client_request,
                                       nb::handle response, nb::handle auth, nb::handle anonymous_auth,
                                       nb::handle basic_auth, nb::handle digest_auth, nb::handle negotiate_auth,
                                       nb::handle ntlm_auth, nb::handle bearer_auth, nb::handle header_auth) {
        bg::web::export_native_schema<bg::web::HttpPeer>(peer);
        bg::web::export_native_schema<bg::web::HttpServerRequest>(server_request);
        bg::web::export_native_schema<bg::web::HttpClientRequest>(client_request);
        bg::web::export_native_schema<bg::web::HttpResponse>(response);
        bg::web::export_native_schema<bg::web::Auth>(auth);
        bg::web::export_native_schema<bg::web::AnonymousAuth>(anonymous_auth);
        bg::web::export_native_schema<bg::web::BasicAuth>(basic_auth);
        bg::web::export_native_schema<bg::web::DigestAuth>(digest_auth);
        bg::web::export_native_schema<bg::web::NegotiateAuth>(negotiate_auth);
        bg::web::export_native_schema<bg::web::NtlmAuth>(ntlm_auth);
        bg::web::export_native_schema<bg::web::BearerAuth>(bearer_auth);
        bg::web::export_native_schema<bg::web::HeaderAuth>(header_auth);
    });
    module.def("_http_server_impl", &bg::web::make_http_server_implementation, nb::arg("routes"),
               nb::arg("responses"), nb::arg("host") = "127.0.0.1", nb::arg("port") = 0);
    module.def("_http_response_field", &bg::web::make_http_response_field, nb::arg("result"));
    module.def("_http_authenticate_requests", &bg::web::make_http_authenticate_requests, nb::arg("requests"),
               nb::arg("decisions"));
    module.def("_http_merge_responses", &bg::web::make_http_merge_responses, nb::arg("responses"),
               nb::arg("rejections"));
    module.def("_http_handler_input_is_dictionary", [](const bg::python::PythonGraphDefinition &handler) {
        return bg::web::details::http_handler_input_is_dictionary(handler);
    });
    module.def("_http_client_impl", &bg::web::make_http_client_impl, nb::arg("requests"), nb::arg("timeout_ms"),
               nb::arg("max_response_body_size"));
}
