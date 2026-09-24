#include <bg/mesh.hpp>

#include <bg/graph/details/mesh_node.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_record.hpp>
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

struct MeshSelectionWiring {
    std::vector<const TimeSeriesType *> requested_types;
    const TimeSeriesType *output_type{};
    bool complete{};

    void validate(const TimeSeriesType &requested) const {
        if (!requested.is_resolved()) {
            throw WiringError{"mesh()[key] requires its receiving input to determine the selected type"};
        }
        if (complete && (output_type == nullptr || !requested.accepts(*output_type))) {
            throw WiringError{"mesh()[key] requests a type that does not accept the mesh branch output"};
        }
    }

    void finish(const TimeSeriesType *output) {
        output_type = output;
        complete = true;
        for (const auto *requested : requested_types) {
            validate(*requested);
        }
    }
};

util::thread_local_pointer &active_mesh_selection_wiring() {
    static auto active = util::thread_local_pointer{};
    return active;
}

class MeshSelectionWiringScope {
  public:
    explicit MeshSelectionWiringScope(std::string name)
        : state_{std::make_shared<MeshSelectionWiring>()},
          previous_{static_cast<MeshSelectionWiringScope *>(active_mesh_selection_wiring().get())},
          name_{std::move(name)} {
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

    const std::shared_ptr<MeshSelectionWiring> &state() const noexcept { return state_; }
    MeshSelectionWiringScope *previous() const noexcept { return previous_; }
    std::string_view name() const noexcept { return name_; }

  private:
    std::shared_ptr<MeshSelectionWiring> state_;
    MeshSelectionWiringScope *previous_;
    std::string name_;
};

class MeshSelectionWiringRecord final : public WiringRecord {
  public:
    MeshSelectionWiringRecord(WiringArgument key, std::shared_ptr<MeshSelectionWiring> state,
                              std::size_t mesh_depth)
        : WiringRecord{TS_T::type()}, key_{std::move(key)}, state_{std::move(state)},
          mesh_depth_{mesh_depth} {}

  private:
    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        const auto &target_type = requested_type.target_type();
        state_->validate(target_type);

        state_->requested_types.push_back(&target_type);
        const auto dependency = wire_mesh_dependency(key_, mesh_depth_, target_type);
        return materialize_wiring_point(dependency, requested_type);
    }

    WiringArgument key_;
    std::shared_ptr<MeshSelectionWiring> state_;
    std::size_t mesh_depth_;
};

const TimeSeriesType *mesh_output_type(const ResolvedSignature &signature) {
    if (signature.output == nullptr) {
        return nullptr;
    }

    const auto &dictionary = type_cast<TimeSeriesDictionaryType>(*signature.output);
    const auto &reference = type_cast<TimeSeriesReferenceType>(dictionary.value_type());
    return &reference.target_type();
}

Signature default_mesh_signature(bool output) {
    auto parameters = std::vector<Parameter>{
        {util::symbol{"function"}, &GraphArgument::type()},
        {util::symbol{"args"}, &TSB<MESH_POSITIONAL>::type(), active, required, false,
         VariadicMode::positional, variadic_wiring_operations<TSB<MESH_POSITIONAL>>()},
        {util::symbol{"kwargs"}, &TSB<MESH_NAMED>::type(), active, required, false,
         VariadicMode::named, variadic_wiring_operations<TSB<MESH_NAMED>>()},
    };
    if (!output) {
        return {util::symbol{"default_mesh"}, std::move(parameters)};
    }

    const auto &result = TimeSeriesVariableInstance<TS_T>::get();
    auto resolvers = std::vector<VariableResolver>{VariableResolver{
        &result,
        [](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
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

    const Signature &signature() const noexcept final { return signature_; }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const final {
        return DefaultMeshWiring::from(arguments.front().graph()).wire();
    }

    Signature signature_;
};

class DefaultSinkMeshTemplate final : public WiringTemplate {
  public:
    DefaultSinkMeshTemplate() : signature_{default_mesh_signature(false)} {}

    const Signature &signature() const noexcept final { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        const auto *function = arguments.graph(0);
        return function != nullptr && DefaultMeshWiring::from(*function).outer_signature.output == nullptr
                   ? accept
                   : reject("the mesh branch graph produces an output");
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
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

    return add_wiring_record(std::make_unique<MeshSelectionWiringRecord>(
        std::move(key), target->state(), depth));
}

GraphArgument build_mesh_graph_argument(const WiringTemplate &function, ResolvedMapCall call,
                                        bool preserve_named_arguments, std::string name) {
    auto scope = MeshSelectionWiringScope{std::move(name)};
    auto built = build_map_graph_argument_wiring(
        util::symbol{"mesh"}, function, std::move(call), preserve_named_arguments);
    scope.state()->finish(mesh_output_type(built.outer_signature));

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
    const auto wiring = graph_argument_access::payload<DefaultMeshWiring>(argument);
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
        const auto materialized = materialize_wiring_point(key_set.time_series(), key_set.time_series().type());
        const auto &key_set_type = static_cast<const TimeSeriesType &>(
            *outer_signature.parameters[*dictionary_key_set_parameter].type);
        key_set = WiringArgument{wiring_point_for_item(materialized, key_set_item_id, key_set_type)};
    }

    return owned(std::span<const WiringArgument>{arguments});
}

} // namespace bg::details

namespace bg {

operator_<auto(GRAPH_T, TSB<details::MESH_POSITIONAL>, TSB<details::MESH_NAMED>) -> TS_T,
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
