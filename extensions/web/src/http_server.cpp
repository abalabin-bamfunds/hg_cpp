#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/map.hpp>
#include <bg/map_argument.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/web/http_server.hpp>

#include "http_server_internal.hpp"
#include "runtime_logging.hpp"
#include "server_output.hpp"
#include "server_runtime.hpp"

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bg::web {
namespace {

using namespace bg::literals;

struct ServerSource {
    Scalar<Opaque<ServerOptions>> options;
    State<ServerRuntime *> runtime_state;
    Out<TSB<details::ServerOutput>> output;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;

    void start(EvaluationContext *context) const {
        auto output_queue = context->runtime_graph().make_queue(context->node_index());
        auto runtime = std::make_unique<ServerRuntime>(options.value(), std::move(output_queue));
        output->runtime.set(runtime.get());
        *runtime_state = runtime.release();
    }

    void stop() const noexcept { delete std::exchange(*runtime_state, nullptr); }

    void finalize() const noexcept { delete std::exchange(*runtime_state, nullptr); }
};

struct ServerMessagesSource {
    In<TS<Opaque<ServerRuntime *>>, passive> runtime;
    Out<TSD<i64, TSD<i64, TS<List<bytes>>>>> messages;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;
    static constexpr auto queue_mode = QueueMode::batch;

    void start(EvaluationContext *context) const {
        runtime.value()->attach_websocket_queue(context->runtime_graph().make_queue(context->node_index()));
    }
};

struct ServerRoutes {
    In<TSD<i64, TS<string>>, optional> routes;
    In<TSD<i64, TS<string>>, optional> websocket_routes;
    In<TS<Opaque<ServerRuntime *>>> runtime;
    Out<TS<i64>> port;

    static constexpr bool deduplicate_wiring = false;

    void evaluate() const {
        auto *server = runtime.value();
        if (routes.valid()) {
            const auto update_route = [&](const Scalar<i64> &client_id) {
                try {
                    const auto route = routes[client_id];
                    if (route.valid()) {
                        server->register_http_route(client_id.value(), std::string{route.value()});
                    } else {
                        server->unregister_http_route(client_id.value());
                    }
                } catch (...) {
                    details::log_runtime_failure("HTTP route update failed", std::current_exception());
                }
            };
            routes.for_each_modified(update_route);
            routes.for_each_removed(
                [&](const Scalar<i64> &client_id) {
                    try {
                        server->unregister_http_route(client_id.value());
                    } catch (...) {
                        details::log_runtime_failure("HTTP route removal failed", std::current_exception());
                    }
                });
        }

        if (websocket_routes.valid()) {
            const auto update_route = [&](const Scalar<i64> &client_id) {
                try {
                    const auto route = websocket_routes[client_id];
                    if (route.valid()) {
                        server->register_websocket_route(client_id.value(), std::string{route.value()});
                    } else {
                        server->unregister_websocket_route(client_id.value());
                    }
                } catch (...) {
                    details::log_runtime_failure("WebSocket route update failed", std::current_exception());
                }
            };
            websocket_routes.for_each_modified(update_route);
            websocket_routes.for_each_removed(
                [&](const Scalar<i64> &client_id) {
                    try {
                        server->unregister_websocket_route(client_id.value());
                    } catch (...) {
                        details::log_runtime_failure("WebSocket route removal failed", std::current_exception());
                    }
                });
        }

        if (!port.valid()) {
            server->start();
            port.set(static_cast<i64>(server->port()));
        }
    }
};

const auto response_sink = node([](In<TS<Opaque<ServerRuntime *>>, passive> server,
                                   In<TSD<i64, TSD<i64, TS<Bundle<HttpResponse>>>>, optional> responses) {
    auto *runtime = server.value();
    if (!responses.valid()) return;

    const auto apply_client = [&](const Scalar<i64> &client_id) {
        try {
            const auto client_responses = responses[client_id];
            if (!client_responses.valid()) return;
            const auto respond = [&](const Scalar<i64> &request_id) {
                try {
                    const auto response = client_responses[request_id];
                    if (!response.valid()) return;
                    runtime->respond(client_id.value(), request_id.value(),
                                     ScalarValue<Bundle<HttpResponse>>{response.value()});
                } catch (...) {
                    details::log_runtime_failure("HTTP response submission failed", std::current_exception());
                }
            };
            client_responses.for_each_modified(respond);
        } catch (...) {
            details::log_runtime_failure("HTTP response batch failed", std::current_exception());
        }
    };
    responses.for_each_modified(apply_client);
});

const auto websocket_response_sink = node(
    [](In<TS<Opaque<ServerRuntime *>>, passive> server,
       In<TSD<i64, TSD<i64, TSB<WebSocketResponse<bytes>::schema>>>, optional> responses) {
        auto *runtime = server.value();
        if (responses.valid()) {
            responses.for_each_modified([&](const Scalar<i64> &client_id) {
                try {
                    const auto client_responses = responses[client_id];
                    if (!client_responses.valid()) return;
                    client_responses.for_each_modified([&](const Scalar<i64> &connection_id) {
                        try {
                            const auto response = client_responses[connection_id];
                            if (!response.valid()) return;

                            auto update = WebSocketResponseUpdate{};
                            if (response.item_updated(0)) {
                                const auto connect_response = response->connect_response;
                                if (connect_response.valid()) {
                                    update.connect_response.emplace(connect_response.value());
                                }
                            }
                            if (response.item_updated(1)) {
                                const auto message = response->message;
                                if (message.valid()) {
                                    update.message.emplace(message.value());
                                }
                            }
                            if (response.item_updated(2)) {
                                const auto close = response->close;
                                if (close.valid()) {
                                    update.close.emplace(close.value());
                                }
                            }
                            if (update.connect_response || update.message || update.close) {
                                runtime->respond_websocket(client_id.value(), connection_id.value(), std::move(update));
                            }
                        } catch (...) {
                            details::log_runtime_failure("WebSocket response submission failed", std::current_exception());
                        }
                    });
                } catch (...) {
                    details::log_runtime_failure("WebSocket response batch failed", std::current_exception());
                }
            });
        }
    });

const auto server_routes = node<ServerRoutes>();
const auto server_source = node<ServerSource>();
const auto server_messages_source = node<ServerMessagesSource>();

bool is_response_dictionary(const TimeSeriesType &type) {
    return TSD<i64, TS<Bundle<HttpResponse>>>::type().accepts(type.target_type());
}

bool is_websocket_response_dictionary(const TimeSeriesType &type) {
    return TSD<i64, TSB<WebSocketResponse<bytes>::schema>>::type().accepts(type.target_type());
}

WiringPoint project_auth_decision_field(WiringPoint requests, WiringPoint decisions, util::symbol field_name) {
    using DECISION = TimeSeriesVariable<"HTTP_AUTH_DECISION">;

    const auto select_field = graph([](const WiringArguments &arguments, wp<TS_T> field,
                                       wp<TS<bool>> enabled) {
        return operators::if_then_else(enabled, field, operators::nothing[arguments.type_map()]());
    });
    const auto project_field = graph<DECISION>(
        {"decision"_n, "request"_n, "name"_n},
        [select_field](wp<DECISION> decision, wp<TS<Bundle<HttpServerRequest>>> request, util::symbol name) {
            return select_field(operators::getattr(decision, name), operators::valid(request));
        });

    return map(project_field, decisions, requests, std::move(field_name), "__valid_items__"_n = true);
}

WiringPoint response_field(WiringPoint result) {
    if (is_response_dictionary(result.type())) return result;

    const auto &result_type = result.type().target_type();
    const auto *bundle = type_cast<TimeSeriesBundleType>(&result_type);
    if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&result_type)) {
        bundle = type_cast<TimeSeriesBundleType>(&dictionary->value_type().target_type());
        if (bundle == nullptr) {
            throw WiringError{"an HTTP handler output dictionary must contain bundle references"};
        }

        const auto response_index = bundle->find_item_index(util::symbol{"response"});
        if (!response_index || !TS<Bundle<HttpResponse>>::type().accepts(bundle->item_type(*response_index))) {
            throw WiringError{"an HTTP handler output bundle must contain a response field"};
        }

        const auto field = map(operators::getattr, result, util::symbol{"response"});
        if (!is_response_dictionary(field.type())) {
            throw WiringError{"an HTTP handler response field must be a response dictionary"};
        }
        return field;
    }

    if (bundle == nullptr) {
        throw WiringError{"an HTTP handler output must be a response dictionary or contain a response field"};
    }
    const auto response_index = bundle->find_item_index(util::symbol{"response"});
    if (!response_index) {
        throw WiringError{"an HTTP handler output bundle must contain a response field"};
    }
    const auto field = operators::getattr(result, util::symbol{"response"});
    if (!is_response_dictionary(field.type())) {
        throw WiringError{"an HTTP handler response field must be a response dictionary"};
    }
    return field;
}

WiringPoint websocket_response_field(WiringPoint result) {
    if (is_websocket_response_dictionary(result.type())) return result;

    const auto &result_type = result.type().target_type();
    const auto *bundle = type_cast<TimeSeriesBundleType>(&result_type);
    if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&result_type)) {
        bundle = type_cast<TimeSeriesBundleType>(&dictionary->value_type().target_type());
        if (bundle == nullptr || !bundle->find_item_index(util::symbol{"response"})) {
            throw WiringError{"a WebSocket handler output dictionary must contain a response field"};
        }
        const auto field = map(operators::getattr, result, util::symbol{"response"});
        if (!is_websocket_response_dictionary(field.type())) {
            throw WiringError{"a WebSocket handler response field must be a response dictionary"};
        }
        return field;
    }

    if (bundle == nullptr || !bundle->find_item_index(util::symbol{"response"})) {
        throw WiringError{"a WebSocket handler output must be a response dictionary or contain a response field"};
    }
    const auto field = operators::getattr(result, util::symbol{"response"});
    if (!is_websocket_response_dictionary(field.type())) {
        throw WiringError{"a WebSocket handler response field must be a response dictionary"};
    }
    return field;
}

void validate_route(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.find_first_of("?#") != std::string_view::npos) {
        throw std::invalid_argument{"HTTP handler paths must be absolute paths without query or fragment"};
    }
}

} // namespace

WiringPoint details::wire_http_server(ServerOptions options, WiringPoint routes, WiringPoint responses,
                                      WiringPoint websocket_routes, WiringPoint websocket_responses) {
    const auto source = server_source(std::move(options));
    const auto message_output = server_messages_source(source->runtime);
    const auto port = server_routes(routes, websocket_routes, source->runtime);
    response_sink(source->runtime, responses);
    websocket_response_sink(source->runtime, websocket_responses);
    const auto combine_connection = graph(
        [](wp<TS<Bundle<HttpServerRequest>>> connect_request, wp<TS<List<bytes>>> messages) {
            return bundle<WebSocketServerRequest<bytes>::schema>("connect_request"_n = connect_request,
                                                                  "messages"_n = messages);
        });
    const auto combine_connections = graph(
        [combine_connection](wp<TSD<i64, TS<Bundle<HttpServerRequest>>>> connections,
                             wp<TSD<i64, TS<List<bytes>>>> connection_messages) {
            return map(combine_connection, connections, connection_messages, "__key_set_arg__"_n = 0);
        });
    const auto websocket_requests =
        map(combine_connections, source->websocket_requests, message_output, "__key_set_arg__"_n = 0);
    return bundle("requests"_n = source->requests, "websocket_requests"_n = websocket_requests, "port"_n = port);
}

WiringPoint details::wire_http_server(ServerOptions options, WiringPoint routes, WiringPoint responses) {
    const auto websocket_routes = operators::nothing[type<TSD<i64, TS<string>>>]();
    const auto websocket_responses =
        operators::nothing[type<TSD<i64, TSD<i64, TSB<WebSocketResponse<bytes>::schema>>>>]();
    return wire_http_server(std::move(options), routes, responses, websocket_routes, websocket_responses);
}

WiringPoint details::http_authenticate_requests(WiringPoint requests, WiringPoint decisions) {
    const auto request_dictionary = wp<TSD<i64, TS<Bundle<HttpServerRequest>>>>{requests};
    const auto identities = project_auth_decision_field(request_dictionary, decisions, util::symbol{"identity"});
    const auto rejections = project_auth_decision_field(request_dictionary, decisions, util::symbol{"rejection"});

    return bundle("requests"_n = request_dictionary[operators::keys_(identities)], "identities"_n = identities,
                  "rejections"_n = rejections);
}

WiringPoint details::http_merge_responses(WiringPoint responses, WiringPoint rejections) {
    if (!is_response_dictionary(responses.type()) || !is_response_dictionary(rejections.type())) {
        throw WiringError{"HTTP responses must be a request dictionary of response time series"};
    }

    return operators::merge(responses, rejections);
}

WiringPoint details::http_response_field(WiringPoint result) { return response_field(result); }

bool details::http_handler_input_is_dictionary(const WiringTemplate &handler) {
    const auto &signature = handler.signature();
    if (signature.parameter_count() == 0 || signature.output() == nullptr) {
        throw std::invalid_argument{"an HTTP handler must take a request input and return a time series"};
    }
    const auto *input = type_cast<TimeSeriesType>(signature.parameter(0).type);
    if (input == nullptr) {
        throw std::invalid_argument{"an HTTP handler request input must be a time series"};
    }
    return type_cast<TimeSeriesDictionaryType>(&input->target_type()) != nullptr;
}

void register_http_server(ServiceCatalogue &catalogue, ServerOptions options) {
    const auto implementation = graph(
        "name"_n = "http_server_impl", [options = std::move(options)] {
            const auto service_requests = http_handler_service.implementation_requests();
            const auto routes = operators::getattr(service_requests, util::symbol{"route"});
            const auto responses = operators::getattr(service_requests, util::symbol{"responses"});
            const auto websocket_requests = websocket_handler_service.implementation_requests();
            const auto websocket_routes = operators::getattr(websocket_requests, util::symbol{"route"});
            const auto websocket_responses = operators::getattr(websocket_requests, util::symbol{"responses"});
            const auto outputs = details::wire_http_server(options, routes, responses, websocket_routes,
                                                           websocket_responses);
            http_handler_service.implementation_replies(operators::getattr(outputs, util::symbol{"requests"}));
            websocket_handler_service.implementation_replies(
                operators::getattr(outputs, util::symbol{"websocket_requests"}));
            http_server_port_service.implementation_replies(operators::getattr(outputs, util::symbol{"port"}));
        });
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(http_handler_service);
    registration.register_impl(websocket_handler_service);
    registration.register_impl(http_server_port_service);
}

wp<TS<i64>> http_server() { return http_server_port_service().template as<TS<i64>>(); }

void details::validate_http_handler(std::string_view path, const WiringTemplate &handler, const Service *policy) {
    validate_route(path);
    (void)details::http_handler_input_is_dictionary(handler);
    if (policy == nullptr) return;

    if (handler.signature().parameter_count() < 2) {
        throw std::invalid_argument{"an authenticated HTTP handler must take request and identity inputs"};
    }

    const auto &policy_signature = policy->signature();
    if (policy_signature.parameter_count() != 1 || policy_signature.output() == nullptr) {
        throw std::invalid_argument{"an HTTP authentication policy must take one request input and return a decision"};
    }
    const auto *policy_input = type_cast<TimeSeriesType>(policy_signature.parameter(0).type);
    if (policy_input == nullptr || !policy_input->accepts(TS<Bundle<HttpServerRequest>>::type()) ||
        type_cast<TimeSeriesType>(policy_signature.output()) == nullptr) {
        throw std::invalid_argument{"an HTTP authentication policy must map TS<HttpServerRequest> to a decision"};
    }
}

wp<> details::wire_http_handler(std::string_view path, const WiringTemplate &handler, const Service *policy,
                              std::span<const WiringArgument> arguments) {
    auto &wiring = WiringContext::current().wiring();
    const auto client_id = service_client_id();
    const auto requests = http_handler_service.client_replies(client_id);
    const auto &handler_input = handler.signature().parameter(0);
    const auto *handler_input_type = type_cast<TimeSeriesType>(handler_input.type);
    const auto *handler_input_dictionary =
        type_cast<TimeSeriesDictionaryType>(&handler_input_type->target_type());

    auto output = WiringPoint{};
    auto handler_requests = requests;
    auto identities = std::optional<WiringPoint>{};
    auto rejections = std::optional<WiringPoint>{};
    if (policy != nullptr) {
        const auto submit_policy_request = graph(
            "name"_n = "http_auth_request",
            [policy](wp<TS<Bundle<HttpServerRequest>>> request)
                -> wp<TS<service_client_id_t, TimeSeriesGuarantee::const_>> {
                const auto client_id = service_client_id();
                const auto request_arguments = std::array{WiringArgument{request}};
                policy->client_requests(TypeMap{}, client_id, request_arguments);
                return client_id;
            });
        const auto request_client_ids = map(submit_policy_request, requests);
        using DECISION = TimeSeriesVariable<"HTTP_AUTH_DECISION">;
        const auto receive_policy_reply = graph<DECISION>(
            "name"_n = "http_auth_reply",
            resolve<DECISION> = [policy]() -> const TimeSeriesType & {
                return *type_cast<TimeSeriesType>(policy->signature().output());
            },
            [policy](wp<TS<service_client_id_t, TimeSeriesGuarantee::const_>> client_id) -> wp<DECISION> {
                const auto replies = policy->client_replies(TypeMap{}, client_id, {});
                return wp<DECISION>::from_validated(*replies);
            });
        const auto decisions = map(receive_policy_reply, request_client_ids);
        const auto projected = details::http_authenticate_requests(requests, decisions);
        handler_requests = operators::getattr(projected, util::symbol{"requests"});
        identities = operators::getattr(projected, util::symbol{"identities"});
        rejections = operators::getattr(projected, util::symbol{"rejections"});
    }

    auto response = WiringPoint{};
    if (handler_input_dictionary != nullptr) {
        if (!handler_input_type->accepts(handler_requests.type())) {
            throw WiringError{"an HTTP dictionary handler input must accept TSD<i64, TS<HttpServerRequest>>"};
        }

        auto handler_arguments = std::vector<WiringArgument>{};
        handler_arguments.reserve(arguments.size() + (identities ? 2 : 1));
        handler_arguments.emplace_back(handler_input.name, handler_requests);
        if (identities) {
            const auto &identity_input = handler.signature().parameter(1);
            const auto *identity_input_type = type_cast<TimeSeriesType>(identity_input.type);
            if (identity_input_type == nullptr || !identity_input_type->accepts(identities->type())) {
                throw WiringError{"an authenticated HTTP dictionary handler must accept an identity dictionary"};
            }
            handler_arguments.emplace_back(identity_input.name, *identities);
        }
        for (const auto &argument : arguments) handler_arguments.emplace_back(argument);
        const auto result = wiring.invoke(handler, handler_arguments);
        if (!result) throw WiringError{"an HTTP handler must return a response time series"};
        output = *result;
        response = response_field(output);
    } else {
        const auto &request_dictionary = type_cast<TimeSeriesDictionaryType>(handler_requests.type().target_type());
        if (!handler_input_type->accepts(request_dictionary.value_type())) {
            throw WiringError{"an HTTP single-request handler input must accept TS<HttpServerRequest>"};
        }
        if (type_cast<TimeSeriesType>(handler.signature().output()) == nullptr) {
            throw WiringError{"an HTTP single-request handler must return a time series"};
        }

        auto map_arguments = std::vector<MapArgument>{};
        map_arguments.reserve(arguments.size() + (identities ? 2 : 1));
        map_arguments.emplace_back(WiringArgument{handler_requests}, false);
        if (identities) {
            const auto &identity_input = handler.signature().parameter(1);
            const auto *identity_input_type = type_cast<TimeSeriesType>(identity_input.type);
            const auto &identity_dictionary = type_cast<TimeSeriesDictionaryType>(identities->type().target_type());
            if (identity_input_type == nullptr || !identity_input_type->accepts(identity_dictionary.value_type())) {
                throw WiringError{"an authenticated HTTP single-request handler must accept an identity value"};
            }
            map_arguments.emplace_back(WiringArgument{*identities}, false);
        }
        for (const auto &argument : arguments) map_arguments.emplace_back(argument, true);
        output = map(handler, std::span<const MapArgument>{map_arguments});
        response = response_field(output);
    }

    if (rejections) {
        response = details::http_merge_responses(response, *rejections);
    }

    auto route = operators::const_(ScalarValue<string>{std::string{path}});
    http_handler_service.client_requests(client_id, route, response);
    return wp<>{output};
}

void details::validate_websocket_handler(std::string_view path, const WiringTemplate &handler, const Service *policy) {
    validate_route(path);
    const auto &signature = handler.signature();
    if (signature.parameter_count() == 0 || signature.output() == nullptr) {
        throw std::invalid_argument{"a WebSocket handler must take a request input and return a time series"};
    }
    const auto *input = type_cast<TimeSeriesType>(signature.parameter(0).type);
    if (input == nullptr) {
        throw std::invalid_argument{"a WebSocket handler request input must be a time series"};
    }
    const auto &target = input->target_type();
    if (type_cast<TimeSeriesDictionaryType>(&target) == nullptr &&
        !input->accepts(TSB<WebSocketServerRequest<bytes>::schema>::type())) {
        throw std::invalid_argument{"a WebSocket handler input must be one request or a request dictionary"};
    }
    if (policy == nullptr) return;

    if (signature.parameter_count() < 2) {
        throw std::invalid_argument{"an authenticated WebSocket handler must take request and identity inputs"};
    }
    const auto &policy_signature = policy->signature();
    if (policy_signature.parameter_count() != 1 || policy_signature.output() == nullptr) {
        throw std::invalid_argument{"a WebSocket authentication policy must take one request input and return a decision"};
    }
    const auto *policy_input = type_cast<TimeSeriesType>(policy_signature.parameter(0).type);
    if (policy_input == nullptr || !policy_input->accepts(TS<Bundle<HttpServerRequest>>::type()) ||
        type_cast<TimeSeriesType>(policy_signature.output()) == nullptr) {
        throw std::invalid_argument{"a WebSocket authentication policy must map TS<HttpServerRequest> to a decision"};
    }
}

wp<> details::wire_websocket_handler(std::string_view path, const WiringTemplate &handler, const Service *policy,
                                     std::span<const WiringArgument> arguments) {
    auto &wiring = WiringContext::current().wiring();
    const auto client_id = service_client_id();
    const auto requests = websocket_handler_service.client_replies(client_id);
    const auto &handler_input = handler.signature().parameter(0);
    const auto *handler_input_type = type_cast<TimeSeriesType>(handler_input.type);
    const auto *handler_input_dictionary =
        type_cast<TimeSeriesDictionaryType>(&handler_input_type->target_type());

    auto handler_requests = requests;
    auto identities = std::optional<WiringPoint>{};
    auto decisions = std::optional<WiringPoint>{};
    if (policy != nullptr) {
        const auto connect_requests = map(operators::getattr, requests, util::symbol{"connect_request"});
        const auto submit_policy_request = graph(
            "name"_n = "websocket_auth_request",
            [policy](wp<TS<Bundle<HttpServerRequest>>> request)
                -> wp<TS<service_client_id_t, TimeSeriesGuarantee::const_>> {
                const auto policy_client_id = service_client_id();
                const auto request_arguments = std::array{WiringArgument{request}};
                policy->client_requests(TypeMap{}, policy_client_id, request_arguments);
                return policy_client_id;
            });
        const auto request_client_ids = map(submit_policy_request, connect_requests);
        using DECISION = TimeSeriesVariable<"WEBSOCKET_AUTH_DECISION">;
        const auto receive_policy_reply = graph<DECISION>(
            "name"_n = "websocket_auth_reply",
            resolve<DECISION> = [policy]() -> const TimeSeriesType & {
                return *type_cast<TimeSeriesType>(policy->signature().output());
            },
            [policy](wp<TS<service_client_id_t, TimeSeriesGuarantee::const_>> policy_client_id) -> wp<DECISION> {
                const auto replies = policy->client_replies(TypeMap{}, policy_client_id, {});
                return wp<DECISION>::from_validated(*replies);
            });
        decisions = map(receive_policy_reply, request_client_ids);
        identities = project_auth_decision_field(connect_requests, *decisions, util::symbol{"identity"});
        handler_requests = requests[operators::keys_(*identities)];
    }

    auto output = WiringPoint{};
    auto handler_responses = WiringPoint{};
    if (handler_input_dictionary != nullptr) {
        if (!handler_input_type->accepts(handler_requests.type())) {
            throw WiringError{"a WebSocket dictionary handler must accept TSD<i64, TSB<WebSocketServerRequest>>"};
        }
        auto handler_arguments = std::vector<WiringArgument>{};
        handler_arguments.reserve(arguments.size() + (identities ? 2 : 1));
        handler_arguments.emplace_back(handler_input.name, handler_requests);
        if (identities) {
            const auto &identity_input = handler.signature().parameter(1);
            const auto *identity_input_type = type_cast<TimeSeriesType>(identity_input.type);
            if (identity_input_type == nullptr || !identity_input_type->accepts(identities->type())) {
                throw WiringError{"an authenticated WebSocket dictionary handler must accept an identity dictionary"};
            }
            handler_arguments.emplace_back(identity_input.name, *identities);
        }
        for (const auto &argument : arguments) handler_arguments.emplace_back(argument);
        const auto result = wiring.invoke(handler, handler_arguments);
        if (!result) throw WiringError{"a WebSocket handler must return a response time series"};
        output = *result;
        handler_responses = websocket_response_field(output);
    } else {
        const auto &request_dictionary = type_cast<TimeSeriesDictionaryType>(handler_requests.type().target_type());
        if (!handler_input_type->accepts(request_dictionary.value_type())) {
            throw WiringError{"a WebSocket single-request handler must accept TSB<WebSocketServerRequest>"};
        }
        if (type_cast<TimeSeriesType>(handler.signature().output()) == nullptr) {
            throw WiringError{"a WebSocket single-request handler must return a time series"};
        }
        auto map_arguments = std::vector<MapArgument>{};
        map_arguments.reserve(arguments.size() + (identities ? 2 : 1));
        map_arguments.emplace_back(WiringArgument{handler_requests}, false);
        if (identities) {
            const auto &identity_input = handler.signature().parameter(1);
            const auto *identity_input_type = type_cast<TimeSeriesType>(identity_input.type);
            const auto &identity_dictionary = type_cast<TimeSeriesDictionaryType>(identities->type().target_type());
            if (identity_input_type == nullptr || !identity_input_type->accepts(identity_dictionary.value_type())) {
                throw WiringError{"an authenticated WebSocket single-request handler must accept an identity value"};
            }
            map_arguments.emplace_back(WiringArgument{*identities}, false);
        }
        for (const auto &argument : arguments) map_arguments.emplace_back(argument, true);
        output = map(handler, std::span<const MapArgument>{map_arguments});
        handler_responses = websocket_response_field(output);
    }

    if (decisions) {
        using DECISION = TimeSeriesVariable<"WEBSOCKET_AUTH_DECISION">;
        const auto combine_response = graph<DECISION>(
            {"request"_n, "handler_response"_n, "decision"_n},
            [](wp<TSB<WebSocketServerRequest<bytes>::schema>>,
               wp<TSB<WebSocketResponse<bytes>::schema>> handler_response, wp<DECISION> decision) {
                const auto handler_connect_response =
                    operators::getattr(handler_response, util::symbol{"connect_response"});
                const auto rejection = operators::getattr(decision, util::symbol{"rejection"});
                const auto connect_response = operators::if_then_else(operators::valid(rejection), rejection,
                                                                       handler_connect_response);
                const auto message = operators::getattr(handler_response, util::symbol{"message"});
                const auto identity = operators::getattr(decision, util::symbol{"identity"});
                const auto handler_close = operators::getattr(handler_response, util::symbol{"close"});
                auto memory = allocator{};
                const auto policy_close = operators::const_(
                    bundle<WebSocketClose>(memory, "code"_n = i64{1008},
                                           "reason"_n = std::string{"Policy violation"}));
                const auto revoked = operators::or_(operators::valid(rejection),
                                                    operators::not_(operators::valid(identity)));
                const auto close = operators::if_then_else(revoked, policy_close, handler_close);
                return bundle<WebSocketResponse<bytes>::schema>("connect_response"_n = connect_response,
                                                                 "message"_n = message, "close"_n = close);
            });
        handler_responses = map(combine_response, requests, handler_responses, *decisions,
                                "__key_set_arg__"_n = 0);
    }

    const auto route = operators::const_(ScalarValue<string>{std::string{path}});
    websocket_handler_service.client_requests(client_id, route, handler_responses);
    return wp<>{output};
}

} // namespace bg::web
