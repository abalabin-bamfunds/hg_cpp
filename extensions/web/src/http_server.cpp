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
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/web/http_server.hpp>

#include "http_server_internal.hpp"
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

struct ServerRoutes {
    In<TSD<i64, TS<string>>, optional> routes;
    In<TS<Opaque<ServerRuntime *>>> runtime;
    Out<TS<i64>> port;

    static constexpr bool deduplicate_wiring = false;

    void evaluate() const {
        auto *server = runtime.value();
        if (routes.valid()) {
            const auto update_route = [&](const Scalar<i64> &client_id) {
                const auto route = routes[client_id];
                if (route.valid()) {
                    server->register_route(client_id.value(), std::string{route.value()});
                } else {
                    server->unregister_route(client_id.value());
                }
            };
            routes.for_each_added(update_route);
            routes.for_each_modified(update_route);
            routes.for_each_removed([&](const Scalar<i64> &client_id) { server->unregister_route(client_id.value()); });
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
        const auto client_responses = responses[client_id];
        if (!client_responses.valid()) return;
        const auto respond = [&](const Scalar<i64> &request_id) {
            const auto response = client_responses[request_id];
            if (!response.valid()) return;
            runtime->respond(client_id.value(), request_id.value(),
                             ScalarValue<Bundle<HttpResponse>>{response.value()});
        };
        client_responses.for_each_added(respond);
        client_responses.for_each_modified(respond);
    };
    responses.for_each_added(apply_client);
    responses.for_each_modified(apply_client);
});

const auto server_routes = node<ServerRoutes>();
const auto error_sink = node([](In<TS<string>> error) { throw std::runtime_error{std::string{error.value()}}; });
const auto server_source = node<ServerSource>();

using HTTP_AUTH_IDENTITY = ScalarVariable<"HTTP_AUTH_IDENTITY">;
using HTTP_AUTH_GATE_VALUE = ScalarVariable<"HTTP_AUTH_GATE_VALUE">;
using HTTP_AUTH_COPY_ITEM = ScalarVariable<"HTTP_AUTH_COPY_ITEM">;
template <template <typename, auto...> typename Field>
using HTTP_AUTH_DECISION_SCHEMA = BundleSchemaVariableDeclaration<"HTTP_AUTH_DECISION_SCHEMA", Field>;
template <template <typename, auto...> typename Field> struct HTTP_AUTH_PROJECTION_SCHEMA {
    Field<REF<TS<Bundle<HttpServerRequest>>>> requests;
    Field<TS<HTTP_AUTH_IDENTITY>> identities;
    Field<TS<Bundle<HttpResponse>>> rejections;
};

const auto http_response_dictionary_reference = node(
    [](In<TSD<i64, REF<TS<Bundle<HttpResponse>>>>, optional> responses,
       Out<TSD<i64, REF<TS<Bundle<HttpResponse>>>>> output) { output.copy_tick(responses); });

bool is_response_dictionary(const TimeSeriesType &type) {
    return TSD<i64, TS<Bundle<HttpResponse>>>::type().accepts(type.target_type());
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

        const auto &response_type = bundle->item_type(*response_index);
        const auto &item_type = dictionary->value_type().target_type();
        const auto &item_reference = time_series_reference_type(item_type, TimeSeriesGuarantee::ordinary);
        auto parameters = std::vector<Parameter>{{util::symbol{"bundle"}, &item_reference}};
        auto signature = Signature{util::symbol{"http_response_field"}, std::move(parameters), response_type};
        const auto projection = GraphTemplate{std::move(signature), [](wp<> value) {
                                                  return operators::getattr(value, util::symbol{"response"});
                                              }};
        const auto arguments = std::array{MapArgument{WiringArgument{result}}};
        return map(projection, std::span<const MapArgument>{arguments});
    }

    if (bundle == nullptr) {
        throw WiringError{"an HTTP handler output must be a response dictionary or contain a response field"};
    }
    const auto response_index = bundle->find_item_index(util::symbol{"response"});
    if (!response_index) {
        throw WiringError{"an HTTP handler output bundle must contain a response field"};
    }
    const auto field = bg::details::time_series_bundle_field(result, util::symbol{"response"});
    if (!is_response_dictionary(field.type())) {
        throw WiringError{"an HTTP handler response field must be a response dictionary"};
    }
    return field;
}

void validate_route(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.find_first_of("?#") != std::string_view::npos) {
        throw std::invalid_argument{"HTTP handler paths must be absolute paths without query or fragment"};
    }
}

} // namespace

WiringPoint details::wire_http_server(ServerOptions options, WiringPoint routes, WiringPoint responses) {
    const auto source = server_source(ScalarValue<Opaque<ServerOptions>>{std::move(options)});
    const auto port = server_routes(routes, source->runtime);
    response_sink(source->runtime, responses);
    error_sink(source->errors);
    return bundle("requests"_n = source->requests, "port"_n = port);
}

WiringPoint details::http_authenticate_requests(WiringPoint requests, WiringPoint decisions) {
    const auto map_field = [](WiringPoint values, util::symbol name) {
        const auto arguments = std::array{MapArgument{WiringArgument{values}},
                                          MapArgument{WiringArgument{util::symbol{"name"}, name}}};
        return map(operators::getattr, std::span<const MapArgument>{arguments});
    };
    const auto identities = map_field(decisions, util::symbol{"identity"});
    const auto rejections = map_field(decisions, util::symbol{"rejection"});

    const auto authenticate_request = graph(
        "name"_n = "http_authenticate_request",
        [](wp<REF<TS<Bundle<HttpServerRequest>>>> request,
           wp<TS<Bundle<HTTP_AUTH_DECISION_SCHEMA>>> decision, wp<TS<HTTP_AUTH_IDENTITY>> identity,
           wp<TS<Bundle<HttpResponse>>> rejection) -> wp<TSB<HTTP_AUTH_PROJECTION_SCHEMA>> {
            const auto request_target = request.as<TS<Bundle<HttpServerRequest>>>();

            static const auto validate = node<HTTP_AUTH_IDENTITY>(
                [](In<TS<Bundle<HttpServerRequest>>, optional> request,
                   In<TS<Bundle<HTTP_AUTH_DECISION_SCHEMA>>, optional> decision,
                   In<TS<HTTP_AUTH_IDENTITY>, optional> identity,
                   In<TS<Bundle<HttpResponse>>, optional> rejection, Out<TS<bool>> output) {
                    if (!request.valid() || !decision.valid()) {
                        output.invalidate();
                        return;
                    }

                    if (identity.valid() == rejection.valid()) {
                        throw std::logic_error{
                            "an authentication decision must contain exactly one of identity or rejection"};
                    }

                    output.set(true);
                });
            const auto valid_decision = validate(request_target, decision, identity, rejection);

            static const auto gate = node<HTTP_AUTH_GATE_VALUE>(
                [](In<TS<bool>, optional> condition, In<TS<HTTP_AUTH_GATE_VALUE>, optional> value,
                   Out<TS<HTTP_AUTH_GATE_VALUE>> output) {
                    if (!condition.valid() || !value.valid()) {
                        output.invalidate();
                        return;
                    }

                    output.copy(value);
                });
            static const auto gate_reference = node<HTTP_AUTH_IDENTITY>(
                [](In<TS<Bundle<HttpServerRequest>>, optional> target,
                   In<REF<TS<Bundle<HttpServerRequest>>>, passive> original,
                   In<TS<HTTP_AUTH_IDENTITY>, optional> condition,
                   Out<REF<TS<Bundle<HttpServerRequest>>>> output) {
                    if (!target.valid() || !condition.valid()) {
                        output.invalidate();
                        return;
                    }

                    output.copy(original);
                });
            const auto accepted_identity = gate(valid_decision, identity);
            const auto accepted_request = gate_reference(request_target, request, accepted_identity);
            const auto accepted_rejection = gate(valid_decision, rejection);

            return bundle<TSB<HTTP_AUTH_PROJECTION_SCHEMA>>(
                "requests"_n = accepted_request, "identities"_n = accepted_identity,
                "rejections"_n = accepted_rejection);
        });
    const auto per_request = map(authenticate_request, requests, decisions, identities, rejections);

    const auto request_values = map_field(per_request, util::symbol{"requests"});
    const auto identity_values = map_field(per_request, util::symbol{"identities"});
    const auto rejection_values = map_field(per_request, util::symbol{"rejections"});

    static const auto copy_valid_references = node<HTTP_AUTH_COPY_ITEM>(
        [](In<TSD<i64, TS<HTTP_AUTH_COPY_ITEM>>, optional> values,
           In<TSD<i64, REF<TS<HTTP_AUTH_COPY_ITEM>>>, optional, passive> references,
           Out<TSD<i64, REF<TS<HTTP_AUTH_COPY_ITEM>>>> output) {
            const auto update = [&](const Scalar<i64> &key) {
                if (values[key].valid()) {
                    output[key].copy(references[key]);
                    return;
                }
                output.erase(key);
            };

            values.for_each_added(update);
            values.for_each_removed([&](const Scalar<i64> &key) { output.erase(key); });
            values.for_each_modified(update);
        });
    const auto accepted_requests =
        copy_valid_references(request_values.as<TSD<i64, TS<HTTP_AUTH_COPY_ITEM>>>(), request_values);
    const auto accepted_identities =
        copy_valid_references(identity_values.as<TSD<i64, TS<HTTP_AUTH_COPY_ITEM>>>(), identity_values);
    const auto accepted_rejections =
        copy_valid_references(rejection_values.as<TSD<i64, TS<HTTP_AUTH_COPY_ITEM>>>(), rejection_values);

    return bundle("requests"_n = accepted_requests, "identities"_n = accepted_identities,
                  "rejections"_n = accepted_rejections);
}

WiringPoint details::http_merge_responses(WiringPoint responses, WiringPoint rejections) {
    const auto normalize_dictionary = [](WiringPoint dictionary) {
        if (!is_response_dictionary(dictionary.type())) {
            throw WiringError{"HTTP responses must be a request dictionary of response time series"};
        }
        if (&dictionary.type().target_type() ==
            &TSD<i64, REF<TS<Bundle<HttpResponse>>, TimeSeriesGuarantee::deduplicated>>::type()) {
            return dictionary;
        }
        return static_cast<WiringPoint>(http_response_dictionary_reference(dictionary));
    };
    const auto response_references = normalize_dictionary(responses);
    const auto rejection_references = normalize_dictionary(rejections);
    return operators::merge(response_references, rejection_references);
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
            const auto outputs = details::wire_http_server(options, routes, responses);
            http_handler_service.implementation_replies(operators::getattr(outputs, util::symbol{"requests"}));
            http_server_port_service.implementation_replies(operators::getattr(outputs, util::symbol{"port"}));
        });
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(http_handler_service);
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

} // namespace bg::web
