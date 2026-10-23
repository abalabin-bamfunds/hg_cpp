#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/map_argument.hpp>
#include <bg/node.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/web/http_server.hpp>

#include "server_output.hpp"
#include "server_runtime.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bg::web {
namespace {

using namespace bg::literals;

struct ServerSource {
    Scalar<string> host;
    i64 port;
    Scalar<List<string>> paths;
    State<ServerRuntime *> runtime_state;
    Out<TSB<details::ServerOutput>> output;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;

    void start(EvaluationContext *context) const {
        auto route_paths = std::vector<std::string>{};
        route_paths.reserve(paths.size());
        for (auto index = std::size_t{}; index < paths.size(); ++index) {
            route_paths.emplace_back(paths[index].value());
        }
        auto options = ServerOptions{.host = std::string{host.value()}, .port = static_cast<std::uint16_t>(port)};
        auto output_queue = context->runtime_graph().make_queue(context->node_index());
        auto runtime = std::make_unique<ServerRuntime>(std::move(options), std::move(route_paths),
                                                       std::move(output_queue));
        output->port.set(static_cast<i64>(runtime->port()));
        output->runtime.set(runtime.get());
        *runtime_state = runtime.release();
    }

    void stop() const noexcept { delete std::exchange(*runtime_state, nullptr); }

    void finalize() const noexcept { delete std::exchange(*runtime_state, nullptr); }
};

const auto route_requests = node([](In<TSD<i64, TS<Bundle<HttpServerRequest>>>, optional> requests, std::string path,
                                    Out<TSD<i64, TS<Bundle<HttpServerRequest>>>> output) {
    if (!requests.modified()) return;

    const auto copy_if_matched = [&](const auto &key) {
        const auto request = requests[key];
        if (!request.valid()) return;
        if (scalar_cast<string>(request.value()[2]).value() == path) output[key].copy_tick(request);
    };
    requests.for_each_added(copy_if_matched);
    requests.for_each_modified(copy_if_matched);
    requests.for_each_removed([&](const auto &key) { (void)output.erase(key); });
});

const auto response_sink = node([](In<TS<Opaque<ServerRuntime *>>, passive> server,
                                   In<TSD<i64, TS<Bundle<HttpResponse>>>> responses) {
    auto *runtime = server.value();
    const auto respond = [&](const Scalar<i64> &key) {
        const auto response = responses[key];
        if (!response.valid()) return;
        runtime->respond(key.value(), ScalarValue<Bundle<HttpResponse>>{response.value()});
    };
    responses.for_each_added(respond);
    responses.for_each_modified(respond);
});

const auto error_sink = node([](In<TS<string>> error) { throw std::runtime_error{std::string{error.value()}}; });
const auto server_source = node<ServerSource>();

void validate_routes(std::span<const Route> routes) {
    if (routes.empty()) {
        throw std::invalid_argument{"an HTTP server requires at least one route"};
    }

    for (auto index = std::size_t{}; index < routes.size(); ++index) {
        const auto path = routes[index].path;
        if (path.empty() || path.front() != '/' || path.find_first_of("?#") != std::string_view::npos) {
            throw std::invalid_argument{"HTTP route paths must be absolute paths without query or fragment"};
        }
        if (std::ranges::any_of(routes.first(index), [path](const Route &route) { return route.path == path; })) {
            throw std::invalid_argument{"HTTP route paths must be unique"};
        }

        const auto &signature = routes[index].handler.signature();
        if (signature.parameter_count() != 1 || signature.output() == nullptr) {
            throw std::invalid_argument{"an HTTP handler must take one time-series input and return a time series"};
        }
    }
}

} // namespace

wp<TS<i64>> http_server(std::span<const Route> routes, ServerOptions options) {
    validate_routes(routes);
    auto path_views = std::vector<std::string_view>{};
    path_views.reserve(routes.size());
    for (const auto &route : routes) {
        path_views.push_back(route.path);
    }
    auto route_paths = ScalarValue<List<string>>{path_views};
    const auto output = server_source(options.host, static_cast<i64>(options.port), route_paths);
    const auto requests = output->requests;
    const auto runtime = output->runtime;

    error_sink(output->errors);

    for (const auto &route : routes) {
        const auto filtered = route_requests(requests, std::string{route.path});
        const auto &signature = route.handler.signature();
        const auto *input_type = type_cast<TimeSeriesType>(signature.parameter(0).type);
        const auto *input_dictionary = type_cast<TimeSeriesDictionaryType>(input_type);
        if (input_dictionary == nullptr) {
            const auto map_arguments = std::array{MapArgument{WiringArgument{filtered}, false}};
            const auto response = map(route.handler, std::span<const MapArgument>{map_arguments});
            response_sink(runtime, response);
        } else {
            const auto response = route.handler(filtered);
            response_sink(runtime, *response);
        }
    }

    return output->port;
}

} // namespace bg::web
