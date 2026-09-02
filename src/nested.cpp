#include <bg/graph/details/nested.hpp>

#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/node.hpp>
#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/scalar/storage_representation.hpp>
#include <bg/time_series/ref/ref_input_view.hpp>
#include <bg/time_series/ref/ref_output_view.hpp>
#include <bg/time_series/ref/ref_storage_representation.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <memory>
#include <stdexcept>
#include <utility>

namespace bg::details {

namespace {

struct NestedInput {
    Out<REF<TS_T>> output;

    void evaluate() const noexcept {}
};

struct NestedOutput {
    In<REF<TS_T>, active, optional> value;
    Out<REF<TS_T>> output;

    void evaluate() const { output.set(value); }
};

const auto nested_input_node = node<NestedInput>();
const auto nested_output_node = node<NestedOutput>();

struct NestedNodeState {
    NestedGraph graph;
};

class NestedNodeDefinition {
  public:
    using compiled_data_type = CompiledNestedGraph;
    using state_type = NestedNodeState;

    static constexpr bool uses_cycle_callbacks = true;
    static constexpr bool uses_future_scheduling = true;

    NestedNodeDefinition(NestedGraphWiring wiring, const ResolvedSignature &graph_signature)
        : wiring_{std::move(wiring)}, output_type_{&static_cast<const TimeSeriesType &>(*graph_signature.output)} {
        auto parameters = std::vector<Parameter>{};
        parameters.reserve(graph_signature.parameters.size() + 1);
        parameters.push_back({util::symbol{"run"},
                              &time_series_value_type(cpp_scalar_type<bool>(), TimeSeriesGuarantee::ordinary), active,
                              required});

        for (const auto &parameter : graph_signature.parameters) {
            if (parameter.type->is_time_series()) {
                auto type = std::make_unique<GenericTimeSeriesReferenceType>(
                    static_cast<const TimeSeriesType &>(*parameter.type), TimeSeriesGuarantee::ordinary);
                parameters.push_back({parameter.name, type.get(), active, optional});
                parameter_types_.push_back(std::move(type));
            } else {
                parameters.push_back({parameter.name, parameter.type, parameter.activity, parameter.validity,
                                      parameter.accepts_absent_scalar});
            }
        }

        reference_output_ = &time_series_reference_type(*output_type_, TimeSeriesGuarantee::ordinary);
        signature_ = std::make_unique<Signature>(util::symbol{"nested"}, std::move(parameters), *reference_output_);
        output_representation_ = &time_series_reference_storage_representation(
            *reference_output_, resolve_time_series_storage_representation(*output_type_));
    }

    [[nodiscard]] const Signature &signature() const { return *signature_; }

    [[nodiscard]] compiled_data_type compile(const ResolvedSignature &, std::pmr::memory_resource &memory) const {
        auto graph = std::allocate_shared<CompiledGraph>(std::pmr::polymorphic_allocator<CompiledGraph>{&memory},
                                                         *wiring_.wiring, memory);
        auto result = compiled_data_type{
            .graph = std::move(graph),
            .input_nodes = std::pmr::vector<std::size_t>{&memory},
            .output_node = wiring_.output_node,
        };
        result.input_nodes.assign(wiring_.input_nodes.begin(), wiring_.input_nodes.end());
        return result;
    }

    [[nodiscard]] const TimeSeriesStorageRepresentation *
    output_storage_representation(const ResolvedSignature &, std::span<const ScalarStorageRepresentation *const>,
                                  std::span<const TimeSeriesStorageRepresentation *const>) const noexcept {
        return output_representation_;
    }

    static void evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
        const auto inputs = invocation.inputs();
        auto input = inputs.begin();
        const auto run_input = (*input).value();
        const auto &run_view = static_cast<const TimeSeriesValueInputView &>(*run_input.operator->());
        const auto run = scalar_cast<bool>(run_view.value_view()).value();
        ++input;

        if (!run) {
            invocation.state().graph.stop(invocation.context());
            invocation.output().invalidate();
            return;
        }

        auto values = std::vector<NestedGraphInput>{};
        values.reserve(invocation.compiled_data().input_nodes.size());
        for (; input != inputs.end(); ++input) {
            const auto value = (*input).value();
            const auto &reference = static_cast<const TimeSeriesReferenceInputView &>(*value.operator->());
            values.push_back(
                {value.valid() ? reference.target() : nullptr, value.valid() ? value.last_updated() : MIN_DT});
        }

        auto &graph = invocation.state().graph;
        if (!graph.running()) {
            graph.start(invocation.compiled_data(), invocation.context(), values);
        } else {
            graph.update_inputs(invocation.compiled_data(), values);
        }
        graph.evaluate(invocation.context());

        auto output = invocation.output();
        static_cast<const TimeSeriesReferenceOutputView &>(*output.operator->())
            .set_target(graph.output(invocation.compiled_data()));
    }

    static void on_invalidation(NodeInvocation<compiled_data_type, state_type> invocation) {
        invocation.state().graph.stop(invocation.context());
        invocation.output().invalidate();
    }

    static void stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        invocation.state().graph.stop(invocation.context());
    }

    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        invocation.state().graph.stop_and_destroy(invocation.context());
    }

  private:
    NestedGraphWiring wiring_;
    const TimeSeriesType *output_type_;
    std::vector<std::unique_ptr<GenericTimeSeriesReferenceType>> parameter_types_;
    const TimeSeriesReferenceType *reference_output_{};
    const TimeSeriesStorageRepresentation *output_representation_{};
    std::unique_ptr<Signature> signature_;
};

} // namespace

WiringPoint wire_nested_input(const TimeSeriesType &type) {
    auto bindings = TypeMap{};
    bindings.set(TimeSeriesVariableInstance<NestedInput, TS_T>::get(), &type);
    return static_cast<WiringPoint>(nested_input_node(bindings));
}

WiringPoint wire_nested_output(WiringPoint output) {
    return static_cast<WiringPoint>(nested_output_node(output));
}

std::unique_ptr<NodeTemplate> make_nested_node(NestedGraphWiring wiring, const ResolvedSignature &graph_signature) {
    return std::make_unique<RuntimeSignatureNodeTemplate<NestedNodeDefinition>>(
        NestedNodeDefinition{std::move(wiring), graph_signature});
}

} // namespace bg::details
