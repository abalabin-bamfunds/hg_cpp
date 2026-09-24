#pragma once

#include <bg/map/details/map_invoke.hpp>
#include <bg/mesh/details/default_mesh_wiring.hpp>
#include <bg/util/named_argument.hpp>

#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace bg {
namespace details {

class MeshDependencySelector {
  public:
    explicit MeshDependencySelector(std::string name = {}) : name_{std::move(name)} {}

    template <typename Key> auto operator[](Key &&key) const {
        return wp<>{mesh_dependency_selection(WiringArgument{std::forward<Key>(key)}, name_)};
    }

  private:
    std::string name_;
};

struct mesh_graph_argument_builder {
    static constexpr auto name = "mesh";

    std::string mesh_name;

    GraphArgument operator()(const WiringTemplate &function, ResolvedMapCall call,
                             bool preserve_named_arguments) const {
        return build_mesh_graph_argument(function, std::move(call), preserve_named_arguments, mesh_name);
    }
};

struct mesh_operator_implementation {
    template <typename SelectOverload> static MeshDependencySelector invoke(SelectOverload &&) {
        return MeshDependencySelector{};
    }

    template <typename SelectOverload, typename Name>
        requires std::convertible_to<Name, std::string_view>
    static MeshDependencySelector invoke(SelectOverload &&, Name &&name) {
        return MeshDependencySelector{std::string{std::string_view{std::forward<Name>(name)}}};
    }

    template <typename SelectOverload, typename Name, typename Graph>
        requires std::convertible_to<Name, std::string_view> &&
                 std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate>
    static auto invoke(SelectOverload &&select_overload, named_argument<"name", Name> metadata, const Graph &function,
                       std::span<const MapArgument> arguments) {
        return invoke_runtime_map(std::forward<SelectOverload>(select_overload), function, arguments,
                                  mesh_graph_argument_builder{std::string{std::string_view{metadata.value}}});
    }

    template <typename SelectOverload, typename Name, typename Graph, typename... Arguments>
        requires std::convertible_to<Name, std::string_view> &&
                 std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate> &&
                 requires { typename std::remove_cvref_t<Graph>::result_type; }
    static auto invoke(SelectOverload &&select_overload, named_argument<"name", Name> metadata, const Graph &function,
                       Arguments &&...arguments) {
        return invoke_map(std::forward<SelectOverload>(select_overload), function,
                          mesh_graph_argument_builder{std::string{std::string_view{metadata.value}}},
                          std::forward<Arguments>(arguments)...);
    }

    template <typename SelectOverload, typename Name, typename Function, typename... Arguments>
        requires std::convertible_to<Name, std::string_view> &&
                 (!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
    static auto invoke(SelectOverload &&select_overload, named_argument<"name", Name> metadata, Function &&function,
                       Arguments &&...arguments) {
        return invoke_map_function(std::forward<SelectOverload>(select_overload), std::forward<Function>(function),
                                   mesh_graph_argument_builder{std::string{std::string_view{metadata.value}}},
                                   std::forward<Arguments>(arguments)...);
    }

    template <typename SelectOverload, typename Graph>
        requires std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate>
    static auto invoke(SelectOverload &&select_overload, const Graph &function,
                       std::span<const MapArgument> arguments) {
        return invoke_runtime_map(std::forward<SelectOverload>(select_overload), function, arguments,
                                  mesh_graph_argument_builder{});
    }

    template <typename SelectOverload, typename Graph, typename... Arguments>
        requires std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate> &&
                 requires { typename std::remove_cvref_t<Graph>::result_type; }
    static auto invoke(SelectOverload &&select_overload, const Graph &function, Arguments &&...arguments) {
        return invoke_map(std::forward<SelectOverload>(select_overload), function, mesh_graph_argument_builder{},
                          std::forward<Arguments>(arguments)...);
    }

    template <typename SelectOverload, typename Function, typename... Arguments>
        requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate> &&
                 !std::convertible_to<Function, std::string_view>)
    static auto invoke(SelectOverload &&select_overload, Function &&function, Arguments &&...arguments) {
        return invoke_map_function(std::forward<SelectOverload>(select_overload), std::forward<Function>(function),
                                   mesh_graph_argument_builder{}, std::forward<Arguments>(arguments)...);
    }
};

} // namespace details
} // namespace bg
