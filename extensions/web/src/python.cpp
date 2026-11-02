#include <hg/python/graph_definition.hpp>
#include <hg/python/schema.hpp>
#include <hg/python/service.hpp>
#include <hg/python/service_catalogue.hpp>
#include <hg/python/wiring_point.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/scalar/bundle/scalar_type.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/type/time_series_bundle_type.hpp>
#include <hg/web/http_client.hpp>
#include <hg/web/http_server.hpp>
#include <hg/web/client_auth.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "http_server_internal.hpp"

namespace nb = nanobind;

namespace hg::web {
namespace {

template <template <template <typename, auto...> typename> typename SCHEMA>
void export_native_schema(nb::handle python_type) {
    const auto &time_series_schema = static_cast<const TimeSeriesBundleType &>(TSB<SCHEMA>::type()).schema();
    const auto &scalar_schema = hg::details::hierarchical_scalar_type<SCHEMA>().schema();
    python::export_schema(python_type, time_series_schema, &scalar_schema);
}

python::PythonWiringPoint make_http_server_implementation(const python::PythonWiringPoint &routes,
                                                           const python::PythonWiringPoint &responses,
                                                           std::string host, std::uint16_t port,
                                                           std::optional<std::string> certificate_chain_file,
                                                           std::optional<std::string> private_key_file) {
    if (certificate_chain_file.has_value() != private_key_file.has_value()) {
        throw std::invalid_argument{"HTTPS requires both certificate_chain_file and private_key_file"};
    }
    auto &wiring = WiringContext::current().wiring();
    auto options = ServerOptions{.host = std::move(host), .port = port};
    if (certificate_chain_file) {
        options.tls = ServerTlsOptions{std::move(*certificate_chain_file), std::move(*private_key_file)};
    }
    const auto output = details::wire_http_server(std::move(options), routes.get(wiring), responses.get(wiring));
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
} // namespace hg::web

NB_MODULE(_hg_web, module) {
    module.def("_register_schemas", [](nb::handle peer, nb::handle server_request, nb::handle client_request,
                                       nb::handle response, nb::handle auth, nb::handle anonymous_auth,
                                       nb::handle basic_auth, nb::handle digest_auth, nb::handle negotiate_auth,
                                       nb::handle ntlm_auth, nb::handle bearer_auth, nb::handle header_auth,
                                       nb::handle client_auth_matcher) {
        hg::web::export_native_schema<hg::web::HttpPeer>(peer);
        hg::web::export_native_schema<hg::web::HttpServerRequest>(server_request);
        hg::web::export_native_schema<hg::web::HttpClientRequest>(client_request);
        hg::web::export_native_schema<hg::web::HttpResponse>(response);
        hg::web::export_native_schema<hg::web::Auth>(auth);
        hg::web::export_native_schema<hg::web::AnonymousAuth>(anonymous_auth);
        hg::web::export_native_schema<hg::web::BasicAuth>(basic_auth);
        hg::web::export_native_schema<hg::web::DigestAuth>(digest_auth);
        hg::web::export_native_schema<hg::web::NegotiateAuth>(negotiate_auth);
        hg::web::export_native_schema<hg::web::NtlmAuth>(ntlm_auth);
        hg::web::export_native_schema<hg::web::BearerAuth>(bearer_auth);
        hg::web::export_native_schema<hg::web::HeaderAuth>(header_auth);
        hg::web::export_native_schema<hg::web::details::ClientAuthMatcher>(client_auth_matcher);
    });
    module.def("_register_services", [](nb::handle client, nb::handle auth_registration,
                                          nb::handle credential_submission) {
        hg::python::export_service(client, hg::web::http_client);
        hg::python::export_service(auth_registration, hg::web::details::client_auth_registration_service);
        hg::python::export_service(credential_submission, hg::web::details::client_credential_submission_service);
    });
    module.def("_http_server_impl", &hg::web::make_http_server_implementation, nb::arg("routes"),
               nb::arg("responses"), nb::arg("host") = "127.0.0.1", nb::arg("port") = 0,
               nb::arg("certificate_chain_file") = nb::none(), nb::arg("private_key_file") = nb::none());
    module.def("_http_response_field", &hg::web::make_http_response_field, nb::arg("result"));
    module.def("_http_authenticate_requests", &hg::web::make_http_authenticate_requests, nb::arg("requests"),
               nb::arg("decisions"));
    module.def("_http_merge_responses", &hg::web::make_http_merge_responses, nb::arg("responses"),
               nb::arg("rejections"));
    module.def("_http_handler_input_is_dictionary", [](const hg::python::PythonGraphDefinition &handler) {
        return hg::web::details::http_handler_input_is_dictionary(handler);
    });
    module.def("_http_client_impl", &hg::web::make_http_client_impl, nb::arg("requests"), nb::arg("timeout_ms"),
               nb::arg("max_response_body_size"));
    module.def("_register_client_transports", [](nb::handle catalogue, std::string scope,
                                                  std::int64_t timeout_ms,
                                                  std::size_t max_response_body_size) {
        hg::web::register_client_transports(
            hg::python::service_catalogue(catalogue), std::move(scope),
            hg::web::HttpClientOptions{.timeout = std::chrono::milliseconds{timeout_ms},
                                       .max_response_body_size = max_response_body_size});
    }, nb::arg("catalogue"), nb::arg("scope"), nb::arg("timeout_ms"), nb::arg("max_response_body_size"));
    module.def("_normalize_client_auth_origin", &hg::web::details::normalize_client_auth_origin,
               nb::arg("origin"));
    module.def("_validate_client_auth_regex", &hg::web::details::validate_client_auth_regex,
               nb::arg("pattern"));
}
