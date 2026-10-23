#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/map_argument.hpp>
#include <bg/node.hpp>
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

class ServerSource {
  public:
    struct CompiledData {
        ServerOptions options;
        std::vector<std::string> paths;
    };

    using compiled_data_type = CompiledData;
    using state_type = ServerRuntime *;
    using Invocation = NodeInvocation<compiled_data_type, state_type>;
    using output_time_series_type = TSB<details::ServerOutput>;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;

    ServerSource(ServerOptions options, std::vector<std::string> paths)
        : options_{std::move(options)}, paths_{std::move(paths)},
          signature_{util::symbol{"http_server"}, {}, TSB<details::ServerOutput>::type()} {}

    const Signature &signature() const noexcept { return signature_; }
    CompiledData compile(const ResolvedSignature &, allocator &) const { return {options_, paths_}; }

    static void start(Invocation invocation) {
        auto output_queue = invocation.context().runtime_graph().make_queue(invocation.context().node_index());
        auto runtime = std::make_unique<ServerRuntime>(invocation.compiled_data().options,
                                                       invocation.compiled_data().paths, std::move(output_queue));

        auto output = invocation.output_as<Out<TSB<details::ServerOutput>>>();
        output->port.set(static_cast<i64>(runtime->port()));
        output->runtime.set(runtime.get());
        invocation.state() = runtime.release();
    }

    static void stop(Invocation invocation) noexcept {
        delete std::exchange(invocation.state(), nullptr);
    }

    static void destroy_state(Invocation invocation) noexcept {
        delete std::exchange(invocation.state(), nullptr);
    }

  private:
    ServerOptions options_;
    std::vector<std::string> paths_;
    Signature signature_;
};

class RouteRequests {
  public:
    using compiled_data_type = std::string;
    using Invocation = NodeInvocation<compiled_data_type>;
    using output_time_series_type = TSD<i64, TS<Bundle<HttpServerRequest>>>;

    static constexpr bool deduplicate_wiring = false;

    explicit RouteRequests(std::string path)
        : path_{std::move(path)},
          signature_{util::symbol{"http_route_requests"},
                     {{.name = util::symbol{"requests"},
                       .type = &TSD<i64, TS<Bundle<HttpServerRequest>>>::type()}},
                     TSD<i64, TS<Bundle<HttpServerRequest>>>::type()} {}

    const Signature &signature() const noexcept { return signature_; }
    compiled_data_type compile(const ResolvedSignature &, allocator &) const { return path_; }

    static void evaluate(Invocation invocation) {
        const auto input = invocation.input<In<TSD<i64, TS<Bundle<HttpServerRequest>>>>>(0);
        auto output = invocation.output_as<Out<TSD<i64, TS<Bundle<HttpServerRequest>>>>>();
        if (!input.valid()) {
            return;
        }

        const auto &path = invocation.compiled_data();
        input.for_each_added([&](const auto &key) {
            const auto request = input[key];
            if (request_matches(request, path)) {
                output[key].copy_tick(request);
            }
        });
        input.for_each_modified([&](const auto &key) {
            const auto request = input[key];
            if (request_matches(request, path)) {
                output[key].copy_tick(request);
            }
        });
        input.for_each_removed([&](const auto &key) { (void)output.erase(key); });
    }

  private:
    static bool request_matches(const In<TS<Bundle<HttpServerRequest>>> &request, std::string_view path) {
        if (!request.valid()) {
            return false;
        }

        const auto value = request.value();
        return scalar_cast<string>(value[2]).value() == path;
    }

    std::string path_;
    Signature signature_;
};

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
    auto paths = std::vector<std::string>{};
    paths.reserve(routes.size());
    for (const auto &route : routes) {
        paths.emplace_back(route.path);
    }

    auto source_output = node(ServerSource{std::move(options), paths})();
    if (!source_output) {
        throw std::invalid_argument{"an HTTP server source must produce output"};
    }
    const auto output = wp<TSB<details::ServerOutput>>{*source_output};
    const auto requests = output->requests;
    const auto runtime = output->runtime;

    static const auto response_sink = node([](In<TS<Opaque<ServerRuntime *>>> server,
                                              In<TSD<i64, TS<Bundle<HttpResponse>>>> responses) {
            if (!server.valid() || !responses.valid() || !responses.modified()) {
                return;
            }
            auto *runtime = server.value();
            const auto respond = [&](const Scalar<i64> &key) {
                const auto response = responses[key];
                if (!response.valid()) {
                    return;
                }
                auto memory = allocator{};
                runtime->respond(key.value(), ScalarValue<Bundle<HttpResponse>>{response.value(), memory});
            };
            responses.for_each_added(respond);
            responses.for_each_modified(respond);
        });
    static const auto error_sink = node([](In<TS<string>> error) {
        if (error.valid() && error.modified()) {
            throw std::runtime_error{std::string{error.value()}};
        }
    });
    error_sink(output->errors);

    for (const auto &route : routes) {
        auto filtered_output = node(RouteRequests{std::string{route.path}})(requests);
        if (!filtered_output) {
            throw std::invalid_argument{"an HTTP route filter must produce requests"};
        }
        const auto filtered = wp<TSD<i64, TS<Bundle<HttpServerRequest>>>>{*filtered_output};
        const auto &signature = route.handler.signature();
        const auto *input_type = signature.parameter_count() == 0
                                     ? nullptr
                                     : type_cast<TimeSeriesType>(signature.parameter(0).type);
        const auto *input_dictionary = input_type == nullptr ? nullptr : type_cast<TimeSeriesDictionaryType>(input_type);
        if (input_dictionary == nullptr) {
            const auto map_arguments = std::array{MapArgument{WiringArgument{filtered}, false}};
            const auto response = map(route.handler, std::span<const MapArgument>{map_arguments});
            response_sink(runtime, response);
        } else {
            const auto response = route.handler(filtered);
            if (!response) {
                throw std::invalid_argument{"an HTTP TSD handler must produce a response"};
            }
            response_sink(runtime, *response);
        }
    }

    return output->port;
}

} // namespace bg::web
