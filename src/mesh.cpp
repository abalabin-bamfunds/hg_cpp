#include <bg/mesh.hpp>

#include <bg/graph/details/mesh_node.hpp>
#include <bg/graph/details/scoped_wiring_template.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/mesh/details/default_mesh_wiring.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_variable.hpp>

#include "thread_local_pointer.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

util::thread_local_pointer &active_mesh_selection_wiring() {
    static auto active = util::thread_local_pointer{};
    return active;
}

class MeshSelectionWiringScope {
  public:
    MeshSelectionWiringScope(std::string_view name, const TimeSeriesType *item_type)
        : previous_{static_cast<MeshSelectionWiringScope *>(active_mesh_selection_wiring().get())}, name_{name},
          item_type_{item_type} {
        if (!name_.empty()) {
            for (auto *scope = previous_; scope != nullptr; scope = scope->previous_) {
                if (scope->name_ == name_) {
                    throw WiringError{"an enclosing mesh already has the name '" + name_ + "'"};
                }
            }
        }
        active_mesh_selection_wiring().set(this);
    }

    ~MeshSelectionWiringScope() { active_mesh_selection_wiring().set(previous_); }

    MeshSelectionWiringScope(const MeshSelectionWiringScope &) = delete;
    MeshSelectionWiringScope &operator=(const MeshSelectionWiringScope &) = delete;

    MeshSelectionWiringScope *previous() const noexcept { return previous_; }
    std::string_view name() const noexcept { return name_; }

    const TimeSeriesType &item_type() const {
        if (item_type_ == nullptr) {
            throw WiringError{"mesh()[key] requires a mesh branch graph output"};
        }
        return *item_type_;
    }

  private:
    MeshSelectionWiringScope *previous_;
    std::string name_;
    const TimeSeriesType *item_type_;
};

Signature default_mesh_signature(bool output) {
    auto parameters = std::vector<Parameter>{
        {util::symbol{"function"}, &GraphArgument::type()},
        {util::symbol{"args"}, &TSB<MESH_POSITIONAL>::type(), active, optional, false, VariadicMode::positional,
         variadic_wiring_operations<TSB<MESH_POSITIONAL>>()},
        {util::symbol{"kwargs"}, &TSB<MESH_NAMED>::type(), active, optional, false, VariadicMode::named,
         variadic_wiring_operations<TSB<MESH_NAMED>>()},
    };
    if (!output) {
        return {util::symbol{"default_mesh"}, std::move(parameters)};
    }

    const auto &result = TimeSeriesVariableInstance<TS_T>::get();
    auto resolvers = std::vector<VariableResolver>{VariableResolver{
        &result, [](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
            const auto *function = arguments.graph(0);
            if (function == nullptr) {
                return static_cast<const Type *>(nullptr);
            }
            return static_cast<const Type *>(DefaultMeshWiring::from(*function).outer_signature.output);
        }}};
    return {util::symbol{"default_mesh"}, std::move(parameters), result, std::move(resolvers)};
}

class DefaultValueMeshTemplate final : public WiringTemplate {
  public:
    DefaultValueMeshTemplate() : signature_{default_mesh_signature(true)} {}

    const Signature &default_signature() const noexcept final { return signature_; }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        return DefaultMeshWiring::from(arguments.front().graph()).wire();
    }

    Signature signature_;
};

class DefaultSinkMeshTemplate final : public WiringTemplate {
  public:
    DefaultSinkMeshTemplate() : signature_{default_mesh_signature(false)} {}

    const Signature &default_signature() const noexcept final { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        const auto *function = arguments.graph(0);
        return function != nullptr && DefaultMeshWiring::from(*function).outer_signature.output == nullptr
                   ? accept
                   : reject("the mesh branch graph produces an output");
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        return DefaultMeshWiring::from(arguments.front().graph()).wire();
    }

    Signature signature_;
};

} // namespace

WiringPoint mesh_dependency_selection(WiringArgument key, std::string_view name) {
    auto *scope = static_cast<MeshSelectionWiringScope *>(active_mesh_selection_wiring().get());
    if (scope == nullptr) {
        throw WiringError{"mesh()[key] is available only while wiring a mesh branch graph"};
    }

    auto *target = scope;
    auto depth = std::size_t{};
    if (!name.empty()) {
        while (target != nullptr && target->name() != name) {
            target = target->previous();
            ++depth;
        }
        if (target == nullptr) {
            throw WiringError{"no enclosing mesh is named '" + std::string{name} + "'"};
        }
    }

    return wire_mesh_dependency(key, depth, target->item_type());
}

GraphArgument build_mesh_graph_argument(const WiringTemplate &function, ResolvedMapCall call,
                                        bool preserve_named_arguments, std::string name, const TypeMap &type_map) {
    const auto branch = ScopedWiringTemplate{
        function,
        [name = std::move(name)](const ResolvedSignature &signature) {
            const auto *output = type_cast<TimeSeriesType>(signature.output);
            return MeshSelectionWiringScope{name, output == nullptr ? nullptr : &output->target_type()};
        },
    };
    auto built = build_map_graph_argument_wiring(util::symbol{"mesh"}, branch, std::move(call),
                                                 preserve_named_arguments, type_map);

    auto structure = built.structure;
    auto default_wiring = std::make_shared<DefaultMeshWiring>(DefaultMeshWiring{
        .graph = std::move(built.graph),
        .outer_signature = std::move(built.outer_signature),
        .outer_arguments = std::move(built.outer_arguments),
        .dictionary_key_set_parameter = built.dictionary_key_set_parameter,
    });
    return GraphArgument{std::move(structure), std::move(default_wiring)};
}

const DefaultMeshWiring &DefaultMeshWiring::from(const GraphArgument &argument) {
    const auto wiring = argument.payload<DefaultMeshWiring>();
    if (!wiring) {
        throw WiringError{"the default mesh overload requires a mesh graph argument"};
    }
    return *wiring;
}

std::optional<WiringPoint> DefaultMeshWiring::wire() const {
    auto node_template = make_mesh_node(graph, outer_signature);
    auto &outer = WiringContext::current().wiring();
    const auto &owned = outer.own_node_template(std::move(node_template));

    auto arguments = outer_arguments;
    if (dictionary_key_set_parameter) {
        auto &key_set = arguments[*dictionary_key_set_parameter];
        const auto &key_set_type =
            static_cast<const TimeSeriesType &>(*outer_signature.parameters[*dictionary_key_set_parameter].type);
        key_set = WiringArgument{outer.output_item(key_set.time_series(), key_set_item_id, key_set_type)};
    }

    auto call = outer.invoke_node(owned, arguments);
    outer.register_nested_service_consumers(*graph.wiring, call.identity);
    return call.output;
}

} // namespace bg::details

namespace bg {

operator_<auto(GRAPH_T, TSB<details::MESH_POSITIONAL>, TSB<details::MESH_NAMED>)->TS_T,
          details::mesh_operator_implementation>
    mesh{"mesh", {"graph", "args", "kwargs"}};

namespace {

const auto register_default_mesh_overloads = [] {
    mesh.overload(details::DefaultValueMeshTemplate{});
    mesh.overload(details::DefaultSinkMeshTemplate{});
    return true;
}();

} // namespace
} // namespace bg
