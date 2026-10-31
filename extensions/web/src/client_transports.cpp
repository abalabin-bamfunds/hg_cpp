#include <bg/feedback.hpp>
#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/library/operators.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/client_auth.hpp>

#include "client_runtime.hpp"
#include "client_request_sink.hpp"
#include "client_source.hpp"
#include "runtime_logging.hpp"
#include "websocket_client_source_replies.hpp"
#include "websocket_client_runtime.hpp"
#include "websocket_client_update_sink.hpp"

#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

namespace bg::web {
namespace {

using namespace bg::literals;

template <template <typename, auto...> typename Field> struct ClientAuthSelection {
    Field<service_client_id_t> registration_id;
    Field<string> error;
};

template <template <typename, auto...> typename Field> struct ClientAuthOrigin {
    Field<string> origin;
    Field<string> error;
};

struct ParseClientAuthOrigin {
    In<TS<string>> url;
    Out<TSB<ClientAuthOrigin>> output;

    void evaluate() const {
        if (!url.modified()) return;

        try {
            output->origin.set(details::client_auth_origin_from_url(url.value()));
            output->error.set("");
        } catch (const std::exception &error) {
            output->origin.invalidate();
            output->error.set(error.what());
        }
    }
};

struct SelectClientAuthRegistration {
    In<TS<string>> origin;
    In<TSD<service_client_id_t, TS<Bundle<details::ClientAuthMatcher>>>, optional> registrations;
    Out<TSB<ClientAuthSelection>> output;

    void evaluate() const {
        auto registration_id = service_client_id_t{};
        auto exact_count = std::size_t{};
        auto regex_count = std::size_t{};
        auto matched_regex_id = service_client_id_t{};
        auto exact_id = service_client_id_t{};
        auto invalid = false;
        const auto origin_value = std::string{origin.value()};

        if (registrations.valid()) registrations.for_each_key_value([&](const auto &key, const auto &matcher) {
            if (invalid) return;
            if (!matcher.valid()) {
                invalid = true;
                return;
            }

            try {
                const auto value = matcher.value();
                const auto pattern = std::string{scalar_cast<string>(value[0]).value()};
                const auto is_regex = scalar_cast<bool>(value[1]).value();
                const auto id = static_cast<service_client_id_t>(*key);
                if (!is_regex) {
                    if (pattern == origin_value) {
                        ++exact_count;
                        exact_id = id;
                    }
                    return;
                }

                const auto expression = std::regex{pattern, std::regex::ECMAScript};
                if (!std::regex_match(origin_value, expression)) return;
                ++regex_count;
                matched_regex_id = id;
            } catch (...) {
                invalid = true;
            }
        });

        auto error = std::string{};
        if (invalid) {
            error = "client auth registration state is invalid";
            details::log_runtime_failure("client auth matcher selection failed", "a matcher is invalid");
        } else if (exact_count > 1 || (exact_count == 0 && regex_count > 1)) {
            error = "multiple client auth registrations match origin";
        } else if (exact_count == 1) {
            registration_id = exact_id;
        } else if (regex_count == 1) {
            registration_id = matched_regex_id;
        }

        output->error.set(error);
        if (error.empty() && (exact_count == 1 || regex_count == 1)) output->registration_id.set(registration_id);
        else output->registration_id.invalidate();
    }
};

const auto parse_client_auth_origin = node<ParseClientAuthOrigin>();
const auto select_client_auth_registration = node<SelectClientAuthRegistration>();

} // namespace

void register_client_transports(ServiceCatalogue &catalogue, std::string scope, HttpClientOptions http_options,
                                WebSocketClientOptions websocket_options) {
    const auto implementation = graph(
        "name"_n = "client_transports_impl",
        [scope, http_options = std::move(http_options),
         websocket_options = std::move(websocket_options)] {
            const auto matcher_requests = operators::getattr(
                details::client_auth_registration_service.implementation_requests(scope), util::symbol{"matcher"});
            const auto credential_requests = operators::getattr(
                details::client_credential_submission_service.implementation_requests(scope),
                util::symbol{"credentials"});
            const auto http_requests =
                operators::getattr(http_client.implementation_requests(), util::symbol{"request"});
            const auto websocket_requests =
                operators::getattr(websocket_client.implementation_requests(), util::symbol{"request"});
            const auto http_source = details::client_source(http_options);
            const auto websocket_output = details::wire_websocket_client_source_replies(websocket_options);

            const auto http_origins = map(
                graph([](wp<TS<Bundle<HttpClientRequest>>> request) {
                    return parse_client_auth_origin(operators::getattr(request, util::symbol{"url"}))->origin;
                }),
                http_requests);
            const auto websocket_origins = map(
                graph([](wp<TSB<BinaryWebSocketClientRequest>> request) {
                    const auto connect = request->connect_request;
                    return parse_client_auth_origin(operators::getattr(connect, util::symbol{"url"}))->origin;
                }),
                websocket_requests);
            const auto demanded_origins =
                opt(operators::keys_(operators::flip(http_origins, false))) |
                opt(operators::keys_(operators::flip(websocket_origins, false)));

            const auto selection_by_origin = map(select_client_auth_registration, demanded_origins,
                                                 "__key_arg__"_n = 0, pass_through(matcher_requests));
            const auto registration_id_by_origin = map(
                graph([](wp<TSB<ClientAuthSelection>> selection) { return selection->registration_id; }),
                selection_by_origin, "__valid_items__"_n = true);
            const auto origins_by_registration = operators::flip(registration_id_by_origin, false);
            details::client_auth_registration_service.implementation_replies(
                feedback(origins_by_registration), scope);

            const auto credentials_by_origin = operators::flip_keys(credential_requests, false);
            const auto http_request_graph = graph(
                "name"_n = "client_auth_http_request",
                [http_options](wp<TS<service_client_id_t>, "key"_n> client_id,
                               wp<TS<Bundle<HttpClientRequest>>, "request"_n> request,
                               wp<TSD<string, TSB<ClientAuthSelection>>> selection_by_origin,
                               wp<TSD<string, TSD<service_client_id_t, REF<TS<Bundle<Auth>>>>> > credentials_by_origin,
                               wp<TS<Opaque<ClientRuntime *>>> runtime) {
                    const auto client_sequence = service_client_id();
                    const auto parsed_origin = parse_client_auth_origin(
                        operators::getattr(request, util::symbol{"url"}));
                    const auto origin = parsed_origin->origin;
                    const auto error = parsed_origin->error;
                    const auto selection = selection_by_origin[origin].as<TSB<ClientAuthSelection>>();
                    const auto selection_error = selection->error;
                    const auto registration_id = selection->registration_id;
                    const auto credentials = credentials_by_origin[origin][registration_id];

                    const auto parse_failed = error != std::string{};
                    const auto resolved_error = operators::if_then_else(parse_failed, error, selection_error);
                    const auto has_error = resolved_error != std::string{};
                    const auto no_registration = !operators::valid(registration_id);
                    const auto ready = operators::valid(resolved_error) &&
                                       (has_error || (no_registration || operators::valid(credentials)));
                    const auto ready_request = operators::filter_(ready, request);

                    details::client_request_sink(client_id, client_sequence, request, ready_request, resolved_error,
                                                 runtime, credentials, http_options);
                });
            map(http_request_graph, http_requests, pass_through(selection_by_origin),
                pass_through(credentials_by_origin),
                http_source->runtime);

            const auto websocket_source = graph(
                "name"_n = "client_auth_websocket_request",
                [websocket_options](wp<TS<service_client_id_t>, "key"_n> client_id,
                   wp<TSB<BinaryWebSocketClientRequest>, "request"_n> request,
                   wp<TSD<string, TSB<ClientAuthSelection>>> selection_by_origin,
                   wp<TSD<string, TSD<service_client_id_t, REF<TS<Bundle<Auth>>>>> > credentials_by_origin,
                   wp<TS<Opaque<WebSocketClientRuntime *>>> runtime) {
                    const auto connect = request->connect_request;
                    const auto message = request->message;
                    const auto close = request->close;
                    const auto parsed_origin = parse_client_auth_origin(
                        operators::getattr(connect, util::symbol{"url"}));
                    const auto origin = parsed_origin->origin;
                    const auto selection = selection_by_origin[origin].as<TSB<ClientAuthSelection>>();
                    const auto selection_error = selection->error;
                    const auto registration_id = selection->registration_id;
                    const auto credentials = credentials_by_origin[origin][registration_id];

                    const auto parse_failed = parsed_origin->error != std::string{};
                    const auto error = operators::if_then_else(parse_failed, parsed_origin->error, selection_error);
                    const auto has_error = error != std::string{};
                    const auto no_registration = !operators::valid(registration_id);
                    const auto ready = operators::valid(error) &&
                                       (has_error || (no_registration || operators::valid(credentials)));
                    const auto ready_connect = operators::filter_(ready, connect);
                    const auto gate_open = operators::last_modified_time(ready_connect) >=
                                           operators::last_modified_time(connect);
                    const auto reset = operators::merge(operators::count(connect), operators::count(close));
                    const auto gated_message = operators::gate(gate_open, message, "reset"_n = reset);

                    details::websocket_client_update_sink(client_id, connect, gated_message, close, ready_connect, error, runtime,
                                                         credentials, websocket_options);
                });
            map(websocket_source, websocket_requests, pass_through(selection_by_origin),
                pass_through(credentials_by_origin), websocket_output->runtime);

            http_client.implementation_replies(http_source->replies);
            websocket_client.implementation_replies(websocket_output->replies);
        });

    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(details::client_auth_registration_service, "scope"_n = scope);
    registration.register_impl(details::client_credential_submission_service, "scope"_n = scope);
    registration.register_impl(http_client);
    registration.register_impl(websocket_client);
}

} // namespace bg::web
