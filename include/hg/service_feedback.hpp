#pragma once

#include <bg/feedback.hpp>
#include <bg/graph/details/graph_signature.hpp>
#include <bg/graph/graph_template.hpp>
#include <bg/graph/service.hpp>
#include <bg/nested.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/with_signature.hpp>

#include <algorithm>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <template <typename, auto...> typename Field>
using SERVICE_FEEDBACK_POSITIONAL = BundleSchemaVariableDeclaration<"SERVICE_FEEDBACK_POSITIONAL", Field>;

template <template <typename, auto...> typename Field>
using SERVICE_FEEDBACK_NAMED = BundleSchemaVariableDeclaration<"SERVICE_FEEDBACK_NAMED", Field>;

using SERVICE_FEEDBACK_OUTPUT = TimeSeriesVariable<"SERVICE_FEEDBACK_OUTPUT">;

} // namespace bg::details

namespace bg {

template <typename Implementation>
    requires std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate> &&
             requires { typename std::remove_cvref_t<Implementation>::result_type; }
[[nodiscard]] auto feedback_service_implementation(const ServiceDefinition &service, Implementation &&implementation) {
    using Graph = std::remove_cvref_t<Implementation>;
    static_assert(details::is_typed_wiring_point<typename Graph::result_type>,
                  "a feedback service implementation must have an output");

    if (!service.implementation_requests_required() || !service.signature().has_output()) {
        throw std::invalid_argument{"feedback requires a service with requests and an output"};
    }

    auto graph = std::make_shared<const Graph>(std::forward<Implementation>(implementation));
    auto code = [source = &service, graph](const WiringArguments &call,
                                           wp<TSB<details::SERVICE_FEEDBACK_POSITIONAL>, var_args> positional,
                                           wp<TSB<details::SERVICE_FEEDBACK_NAMED>, var_kwargs> named)
        -> wp<details::SERVICE_FEEDBACK_OUTPUT> {
        auto supplied = std::vector<WiringArgument>{};
        details::append_wiring_arguments(supplied, args(positional));
        details::append_wiring_arguments(supplied, kwargs(named));

        auto type_map = call.type_map().copy();
        const auto signature = source->signature().complete_resolution(type_map);
        auto next_positional = std::size_t{};
        auto request_index = std::size_t{};
        for (const auto &request : source->implementation_request_inputs(signature)) {
            auto found = supplied.end();
            const auto name = request.name ? *request.name : graph->signature().parameter(request_index).name;
            if (name.has_value()) {
                found = std::ranges::find_if(supplied, [&](const auto &argument) {
                    return argument.name() == name;
                });
            }
            if (found == supplied.end() && !request.name) {
                while (next_positional < supplied.size() && supplied[next_positional].name()) {
                    ++next_positional;
                }
                if (next_positional < supplied.size()) {
                    found = supplied.begin() + static_cast<std::ptrdiff_t>(next_positional++);
                }
            }
            if (found == supplied.end() || !found->is_time_series()) {
                throw WiringError{"a feedback service call is missing an implementation request"};
            }

            auto delayed = WiringArgument{feedback(found->time_series(), *request.type)};
            if (request.name) {
                delayed.set_name(*request.name);
            }
            if (found->requires_optional()) {
                delayed.require_optional();
            }
            *found = std::move(delayed);
            ++request_index;
        }

        const auto bound = details::bind_wiring_arguments(graph->signature(), supplied);
        const auto result = nested((*graph)[call.type_map()], operators::const_(true),
                                   std::span<const WiringArgument>{bound});
        return wp<details::SERVICE_FEEDBACK_OUTPUT>{result};
    };

    const auto &result = details::TimeSeriesVariableInstance<details::SERVICE_FEEDBACK_OUTPUT>::get();
    auto resolver = VariableResolver{&result, [source = &service](const TypeMap &type_map) -> TypeMapValue {
                                         auto resolved_map = type_map.copy();
                                         const auto signature = source->signature().complete_resolution(resolved_map);
                                         return static_cast<const Type *>(source->implementation_output_type(signature));
                                     }};
    auto signature = details::make_graph_signature<decltype(code)>(
        util::symbol{"feedback_service_implementation"}, {}, {std::move(resolver)});
    auto wrapper = GraphTemplate<decltype(code)>{std::move(signature), std::move(code)};
    return with_signature(std::move(wrapper), graph->signature());
}

} // namespace bg
