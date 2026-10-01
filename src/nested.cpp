#include <bg/details/evaluation_context.hpp>
#include <bg/graph/details/nested.hpp>

#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/details/nested_graph_context.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/node.hpp>
#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type.hpp>

#include <memory>
#include <stdexcept>
#include <utility>

namespace bg::details {

namespace {

struct NestedInput {
    static constexpr bool deduplicate_wiring = false;

    Out<REF<TS_T>> output;

    void evaluate() const noexcept {}
};

struct NestedOutput {
    In<REF<TS_T>, active, optional> value;
    Out<REF<TS_T>> output;

    void evaluate() const { output.copy(value); }
};

struct NestedConstantInput {
    static constexpr bool deduplicate_wiring = false;

    Out<TS<SCALAR, TimeSeriesGuarantee::const_>> output;

    void evaluate() const noexcept {}
};

const auto nested_input_node = node<NestedInput>();
const auto nested_constant_input_node = node<NestedConstantInput>();
const auto nested_output_node = node<NestedOutput>();

struct NestedNodeState {
    NestedGraph graph;
};

class NestedNodeDefinition {
  public:
    using compiled_data_type = CompiledNestedGraph;
    using state_type = NestedNodeState;

    static constexpr bool uses_cycle_callbacks = true;
    static constexpr bool uses_timed_scheduling = true;

    NestedNodeDefinition(NestedGraphWiring wiring, const ResolvedSignature &graph_signature)
        : wiring_{std::move(wiring)}, output_type_{&static_cast<const TimeSeriesType &>(*graph_signature.output)} {
        const auto inherited_count = wiring_.wiring->inherited_requirements().size();
        if (inherited_count > graph_signature.parameters.size()) {
            throw std::invalid_argument{"a nested graph has too many inherited time series"};
        }
        const auto inherited_parameter_start = graph_signature.parameters.size() - inherited_count;

        auto parameters = std::vector<Parameter>{};
        parameters.reserve(graph_signature.parameters.size() + 1);
        parameters.push_back({util::symbol{"run"}, &TS<bool>::type(), active, required});

        for (auto parameter_index = std::size_t{}; parameter_index < graph_signature.parameters.size();
             ++parameter_index) {
            const auto &parameter = graph_signature.parameters[parameter_index];
            if (parameter.type->is_time_series()) {
                if (parameter_index >= inherited_parameter_start) {
                    parameters.push_back({parameter.name, parameter.type, passive, optional});
                    continue;
                }
                auto type = std::make_unique<GenericTimeSeriesReferenceType>(
                    static_cast<const TimeSeriesType &>(*parameter.type), TimeSeriesGuarantee::ordinary);
                parameters.push_back({parameter.name, type.get(), active, optional});
                parameter_types_.push_back(std::move(type));
            } else {
                parameters.push_back({parameter.name, parameter.type, parameter.activity, parameter.validity,
                                      parameter.accepts_absent_scalar, details::VariadicMode::none, nullptr, false});
            }
        }

        reference_output_ = &time_series_reference_type(*output_type_, TimeSeriesGuarantee::deduplicated);
        signature_ = std::make_unique<Signature>(util::symbol{"nested"}, std::move(parameters), *reference_output_);
    }

    const Signature &signature() const { return *signature_; }

    NodeTemplateIdentity definition_identity() const {
        auto structure = graph_wiring_structure(*wiring_.wiring, wiring_.input_nodes, wiring_.output_node);
        return NodeTemplateIdentity::from_value(std::move(structure));
    }

    [[nodiscard]] compiled_data_type compile(const ResolvedSignature &, allocator &memory) const {
        auto node_indices = std::vector{wiring_.output_node};
        node_indices.insert(node_indices.end(), wiring_.input_nodes.begin(), wiring_.input_nodes.end());
        auto graph = compile_graph(*wiring_.wiring, memory, node_indices);
        auto result = compiled_data_type{
            .graph = graph,
            .input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
            .constant_input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
            .parent_service_binding_indices = std::pmr::vector<std::uint32_t>{&memory.memory_resource()},
            .output_node = node_indices.front(),
        };
        result.input_nodes.assign(node_indices.begin() + 1, node_indices.end());
        for (const auto &requirement : wiring_.wiring->service_binding_requirements()) {
            result.parent_service_binding_indices.push_back(
                static_cast<std::uint32_t>(*requirement.parent_requirement));
        }
        return result;
    }

    static bool evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &state = invocation.state();
        if (state.graph.evaluation_pending()) {
            return resume(invocation);
        }

        const auto inputs = invocation.inputs();
        auto input = inputs.begin();
        const auto run_input = (*input).value();
        const auto run = *run_input.visit(
            [](const In<TS<SCALAR>> *value) { return scalar_cast<bool>(value->value_view()).value(); }, ddv::noop);
        if (!run) {
            state.graph.stop(invocation.context());
            invocation.output().invalidate();
            return true;
        }

        auto values = std::vector<NestedGraphInput>{};
        values.reserve(invocation.compiled_data().input_nodes.size());
        for (auto index = std::size_t{}; index < invocation.compiled_data().input_nodes.size(); ++index) {
            ++input;
            const auto value = (*input).value();
            const auto reference = *value.visit([](const In<REF<TS_T>> *item) { return item->value(); }, ddv::noop);
            values.push_back({value.valid() ? reference : nullptr, value.valid() ? value.last_updated() : MIN_DT});
        }

        const auto graph_inputs = std::span<const NestedGraphInput>{values};

        auto &graph = state.graph;
        if (!graph.running()) {
            const auto inherited_count = invocation.compiled_data().graph->inherited_time_series_count();
            auto startup_context =
                NestedGraphStartupContext{invocation, invocation.compiled_data(),
                                          invocation.parameter_count() - inherited_count, inherited_count};
            graph.start(invocation.compiled_data(), startup_context.context(), graph_inputs);
        } else {
            graph.update_inputs(invocation.compiled_data(), graph_inputs);
        }
        return resume(invocation);
    }

    static bool resume(NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &state = invocation.state();
        if (!state.graph.evaluate(invocation.context())) {
            return false;
        }

        auto output = invocation.output();
        output.visit(
            [&](Out<REF<TS_T>> *reference) { reference->set_value(state.graph.output(invocation.compiled_data())); },
            ddv::noop);
        return true;
    }

    static void on_invalidation(NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &state = invocation.state();
        state.graph.stop(invocation.context());
        invocation.output().invalidate();
    }

    static void stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        auto &state = invocation.state();
        state.graph.stop(invocation.context());
    }

    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        invocation.state().graph.stop_and_destroy(invocation.context());
    }

  private:
    NestedGraphWiring wiring_;
    const TimeSeriesType *output_type_;
    std::vector<std::unique_ptr<GenericTimeSeriesReferenceType>> parameter_types_;
    const TimeSeriesReferenceType *reference_output_{};
    std::unique_ptr<Signature> signature_;
};

} // namespace

WiringPoint wire_nested_input(const TimeSeriesType &type) {
    auto type_map = TypeMap{};
    type_map.set(TimeSeriesVariableInstance<TS_T>::get(), &type);
    return static_cast<WiringPoint>(nested_input_node(type_map));
}

WiringPoint wire_nested_constant_input(const TimeSeriesType &type) {
    const auto &value = type_cast<TimeSeriesValueType>(type);
    auto type_map = TypeMap{};
    type_map.set(ScalarVariableInstance<SCALAR>::get(), &value.value_type());
    return static_cast<WiringPoint>(nested_constant_input_node(type_map));
}

WiringPoint wire_nested_output(WiringPoint output, const TimeSeriesType &type) {
    auto *resolution_type = &type;
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&output.type())) {
        resolution_type = &time_series_reference_type(type.target_type(), *reference->guarantee().value());
    }

    auto argument = WiringArgument{output};
    wiring_argument_access::set_resolution_type(argument, *resolution_type);
    return static_cast<WiringPoint>(nested_output_node(std::move(argument)));
}

std::unique_ptr<NodeTemplate> make_nested_node(NestedGraphWiring wiring, const ResolvedSignature &graph_signature) {
    return std::make_unique<RuntimeSignatureNodeTemplate<NestedNodeDefinition>>(
        NestedNodeDefinition{std::move(wiring), graph_signature});
}

} // namespace bg::details
