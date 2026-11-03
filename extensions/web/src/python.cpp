#include <hg/python/graph_definition.hpp>
#include <hg/python/schema.hpp>
#include <hg/python/service.hpp>
#include <hg/python/service_catalogue.hpp>
#include <hg/python/wiring_argument.hpp>
#include <hg/python/wiring_point.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/scalar/bundle/scalar_type.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/type/time_series_bundle_type.hpp>
#include <hg/web/http_client.hpp>
#include <hg/web/http_server.hpp>
#include <hg/web/websocket_client.hpp>
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
#include <memory_resource>
#include <optional>
#include <span>
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

template <template <template <typename, auto...> typename> typename SCHEMA>
void export_native_time_series_schema(nb::handle python_type) {
    const auto &time_series_schema = static_cast<const TimeSeriesBundleType &>(TSB<SCHEMA>::type()).schema();
    python::export_schema(python_type, time_series_schema, nullptr);
}

python::PythonWiringPoint make_http_server_implementation(const python::PythonWiringPoint &routes,
                                                           const python::PythonWiringPoint &responses,
                                                           const python::PythonWiringPoint &websocket_routes,
                                                           const python::PythonWiringPoint &websocket_responses,
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
    const auto output = details::wire_http_server(std::move(options), routes.get(wiring), responses.get(wiring),
                                                  websocket_routes.get(wiring), websocket_responses.get(wiring));
    return python::PythonWiringPoint{output, python::PythonWiringScope::current()};
}

python::PythonWiringPoint make_websocket_client_impl(const python::PythonWiringPoint &requests,
                                                      std::int64_t handshake_timeout_ms,
                                                      std::size_t max_message_size,
                                                      std::optional<std::string> ca_file) {
    auto implementation = websocket_client_impl(WebSocketClientOptions{
        .handshake_timeout = std::chrono::milliseconds{handshake_timeout_ms},
        .max_message_size = max_message_size,
        .ca_file = std::move(ca_file),
    });
    const auto argument = requests.argument(WiringContext::current().wiring());
    const auto result = (*implementation)(std::span<const WiringArgument>{&argument, 1});
    return python::PythonWiringPoint{*result, python::PythonWiringScope::current()};
}

python::PythonWiringPoint make_websocket_handler(std::string path,
                                                  const python::PythonGraphDefinition &handler,
                                                  nb::handle policy, nb::sequence fixed_values,
                                                  nb::sequence positional_values, nb::dict named_values) {
    auto &wiring = WiringContext::current().wiring();
    const auto first_ambient_parameter = policy.is_none() ? std::size_t{1} : std::size_t{2};
    const auto &parameter_types = handler.wiring_parameter_types();
    if (first_ambient_parameter > parameter_types.size()) {
        throw std::invalid_argument{"invalid WebSocket handler parameter offset"};
    }

    auto indexes = std::vector<std::size_t>{};
    indexes.reserve(parameter_types.size() - first_ambient_parameter);
    for (auto index = first_ambient_parameter; index < parameter_types.size(); ++index) indexes.push_back(index);
    const auto ambient_signature = handler.signature().select_parameters(indexes);
    const auto ambient_types = parameter_types.subspan(first_ambient_parameter);
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    const auto arguments = python::details::wiring_arguments(
        ambient_signature, fixed_values, positional_values, named_values, memory, wiring, ambient_types);

    const auto native_policy = [&]() -> const Service * {
        if (policy.is_none()) return nullptr;
        const auto &definition = python::service_definition(policy);
        const auto *service = dynamic_cast<const Service *>(&definition);
        if (service == nullptr) {
            throw std::invalid_argument{"a WebSocket authentication policy must be a service declaration"};
        }
        return service;
    }();
    details::validate_websocket_handler(path, handler, native_policy);
    const auto output = details::wire_websocket_handler(path, handler, native_policy, arguments);
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
                                       nb::handle client_auth_matcher, nb::handle websocket_connect_request,
                                       nb::handle websocket_close) {
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
        hg::web::export_native_schema<hg::web::WebSocketConnectRequest>(websocket_connect_request);
        hg::web::export_native_schema<hg::web::WebSocketClose>(websocket_close);
    });
    module.def("_register_websocket_time_series_schemas", [](nb::handle websocket_server_request,
                                                                nb::handle websocket_server_response,
                                                                nb::handle websocket_client_request,
                                                                nb::handle websocket_client_reply) {
        hg::web::export_native_time_series_schema<hg::web::BinaryWebSocketServerRequest>(websocket_server_request);
        hg::web::export_native_time_series_schema<hg::web::BinaryWebSocketResponse>(websocket_server_response);
        hg::web::export_native_time_series_schema<hg::web::BinaryWebSocketClientRequest>(websocket_client_request);
        hg::web::export_native_time_series_schema<hg::web::BinaryWebSocketClientReply>(websocket_client_reply);
    });
    module.def("_register_services", [](nb::handle client, nb::handle websocket_client,
                                          nb::handle websocket_handler,
                                          nb::handle auth_registration,
                                          nb::handle credential_submission) {
        hg::python::export_service(client, hg::web::http_client);
        hg::python::export_service(websocket_client, hg::web::websocket_client);
        hg::python::export_service(websocket_handler, hg::web::websocket_handler_service);
        hg::python::export_service(auth_registration, hg::web::details::client_auth_registration_service);
        hg::python::export_service(credential_submission, hg::web::details::client_credential_submission_service);
    });
    module.def("_http_server_impl", &hg::web::make_http_server_implementation, nb::arg("routes"),
               nb::arg("responses"), nb::arg("websocket_routes"), nb::arg("websocket_responses"),
               nb::arg("host") = "127.0.0.1", nb::arg("port") = 0,
               nb::arg("certificate_chain_file") = nb::none(), nb::arg("private_key_file") = nb::none());
    module.def("_websocket_handler", &hg::web::make_websocket_handler, nb::arg("path"), nb::arg("handler"),
               nb::arg("policy").none(), nb::arg("fixed_values"), nb::arg("positional_values"),
               nb::arg("named_values"));
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
    module.def("_websocket_client_impl", &hg::web::make_websocket_client_impl, nb::arg("requests"),
               nb::arg("handshake_timeout_ms"), nb::arg("max_message_size"), nb::arg("ca_file") = nb::none());
    module.def("_register_client_transports", [](nb::handle catalogue, std::string scope,
                                                  std::int64_t timeout_ms,
                                                  std::size_t max_response_body_size,
                                                  std::int64_t handshake_timeout_ms,
                                                  std::size_t max_message_size,
                                                  std::optional<std::string> ca_file) {
        hg::web::register_client_transports(
            hg::python::service_catalogue(catalogue), std::move(scope),
            hg::web::HttpClientOptions{.timeout = std::chrono::milliseconds{timeout_ms},
                                       .max_response_body_size = max_response_body_size},
            hg::web::WebSocketClientOptions{.handshake_timeout = std::chrono::milliseconds{handshake_timeout_ms},
                                            .max_message_size = max_message_size,
                                            .ca_file = std::move(ca_file)});
    }, nb::arg("catalogue"), nb::arg("scope"), nb::arg("timeout_ms"), nb::arg("max_response_body_size"),
       nb::kw_only(), nb::arg("handshake_timeout_ms") = 30'000,
       nb::arg("max_message_size") = 8 * 1024 * 1024, nb::arg("ca_file") = nb::none());
    module.def("_normalize_client_auth_origin", &hg::web::details::normalize_client_auth_origin,
               nb::arg("origin"));
    module.def("_validate_client_auth_regex", &hg::web::details::validate_client_auth_regex,
               nb::arg("pattern"));
}
