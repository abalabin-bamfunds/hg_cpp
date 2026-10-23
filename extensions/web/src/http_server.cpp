#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/map_argument.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
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

Signature handler_signature(const WiringTemplate &handler) {
    const auto dictionary_input = details::http_handler_input_is_dictionary(handler);
    const auto &signature = handler.signature();
    auto parameters = std::vector<std::size_t>{};
    for (auto index = std::size_t{1}; index < signature.parameter_count(); ++index) parameters.push_back(index);
    auto result = signature.select_parameters(parameters).with_name(util::symbol{"http_handler"});
    if (!dictionary_input) {
        const auto &response_type = time_series_dictionary_type(cpp_scalar_type<i64>(), *signature.output(),
                                                                TimeSeriesGuarantee::ordinary);
        result = result.with_output(response_type);
    }
    return result;
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

HttpHandler::HttpHandler(std::string path, std::shared_ptr<const WiringTemplate> handler)
    : path_{std::move(path)}, handler_{std::move(handler)}, signature_{handler_signature(*handler_)} {
    validate_route(path_);
}

std::optional<WiringPoint> HttpHandler::wire(GraphWiring &wiring, ResolvedSignature,
                                             const WiringArguments &argument_view,
                                             std::span<const WiringArgument> arguments) const {
    const auto client_id = service_client_id();
    const auto requests = http_handler_service.client_replies(client_id);
    const auto &handler_input = handler_->signature().parameter(0);
    const auto *handler_input_type = type_cast<TimeSeriesType>(handler_input.type);
    const auto *handler_input_dictionary =
        type_cast<TimeSeriesDictionaryType>(&handler_input_type->target_type());

    auto output = WiringPoint{};
    auto response = WiringPoint{};
    if (handler_input_dictionary != nullptr) {
        if (!handler_input_type->accepts(requests.type())) {
            throw WiringError{"an HTTP dictionary handler input must accept TSD<i64, TS<HttpServerRequest>>"};
        }

        auto handler_arguments = std::vector<WiringArgument>{};
        handler_arguments.reserve(arguments.size() + 1);
        handler_arguments.emplace_back(handler_input.name, requests);
        for (const auto &argument : arguments) handler_arguments.emplace_back(argument);
        const auto result = wiring.invoke(*handler_, handler_arguments, &argument_view.type_map());
        if (!result) throw WiringError{"an HTTP handler must return a response time series"};
        output = *result;
        response = response_field(output);
    } else {
        const auto &request_dictionary = type_cast<TimeSeriesDictionaryType>(requests.type().target_type());
        if (!handler_input_type->accepts(request_dictionary.value_type())) {
            throw WiringError{"an HTTP single-request handler input must accept TS<HttpServerRequest>"};
        }
        if (type_cast<TimeSeriesType>(handler_->signature().output()) == nullptr) {
            throw WiringError{"an HTTP single-request handler must return a time series"};
        }

        auto map_arguments = std::vector<MapArgument>{};
        map_arguments.reserve(arguments.size() + 1);
        map_arguments.emplace_back(WiringArgument{requests}, false);
        for (const auto &argument : arguments) map_arguments.emplace_back(argument, true);
        output = map(*handler_, std::span<const MapArgument>{map_arguments});
        response = response_field(output);
    }

    auto route = operators::const_(ScalarValue<string>{path_});
    http_handler_service.client_requests(client_id, route, response);
    return output;
}

} // namespace bg::web
