#include "ref_node.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/node/node_template.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace bg::details {
namespace {

class ReferenceNodeTemplate final : public NodeTemplate {
  public:
    ReferenceNodeTemplate(const TimeSeriesType &input_type, const TimeSeriesReferenceType &type)
        : signature_{util::symbol{"reference"}, {{util::symbol{"target"}, &input_type, passive, optional}}, type},
          uses_cycle_callbacks_{input_type.reference_changes_require_cycle_cleanup()} {}

    const Signature &default_signature() const override { return signature_; }

    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return nullptr; }

    NodeFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }

    NodeStateLayout state_layout(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                                 std::span<const ScalarType *const>) const override {
        return {
            .size = sizeof(RootReference),
            .alignment = alignof(RootReference),
            .field_offsets = {},
            .generic_runtime_types = {},
        };
    }

    void construct_initial_state(const NodeStateLayout &, void *storage) const noexcept override {
        std::construct_at(static_cast<RootReference *>(storage));
    }

    NodeFunction state_destruction_function(const ResolvedSignature &) const noexcept override {
        return &destroy_state;
    }

    bool uses_time_series_input_runtime_type(const ResolvedSignature &, std::size_t) const noexcept override {
        return true;
    }

    bool uses_cycle_callbacks() const noexcept override { return uses_cycle_callbacks_; }

    InputNotification input_notification(std::size_t) const noexcept override { return {&notify_input, 0}; }

  private:
    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        const auto runtime_node = Node{node, runtime_storage};
        auto &root = *static_cast<RootReference *>(runtime_node.state());
        auto &output = *static_cast<Reference *>(runtime_node.output_value());

        root.owner_graph = &context->runtime_graph();
        root.set_input_node(node);
        output = &root;
        runtime_node.output_last_updated().publish(context->evaluation_time());
    }

    static void destroy_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        auto &root = *static_cast<RootReference *>(Node{node, runtime_storage}.state());
        auto memory = context->runtime_graph().allocation();
        ReferenceProvider::destroy_root(root, memory);
    }

    static void notify_input(OutputChangeContext &context, std::size_t node_index, std::size_t, OutputChange change) {
        auto &root = *static_cast<RootReference *>(context.runtime_graph().node(node_index).state());
        ReferenceProvider::notify(Reference{&root}, context, change);
    }

    Signature signature_;
    bool uses_cycle_callbacks_;
};

class InheritedReferenceForwarder {
  public:
    explicit InheritedReferenceForwarder(const TimeSeriesReferenceType &type)
        : signature_{util::symbol{"inherited_reference_forwarder"},
                     {{util::symbol{"value"}, &type, active, optional}}, type} {}

    const Signature &signature() const noexcept { return signature_; }

    NodeTemplateIdentity definition_identity() const {
        static constexpr unsigned char identity{};
        return NodeTemplateIdentity::persistent(&identity);
    }

    static void evaluate(NodeInvocation<> invocation) {
        const auto input = invocation.input<In<REF<TS_T>>>(0);
        auto output = invocation.output<REF<TS_T>>();
        output.copy(input);
    }

  private:
    Signature signature_;
};

} // namespace

const NodeTemplate &reference_node_template(const TimeSeriesType &argument, const TimeSeriesReferenceType &type) {
    struct Entry {
        const TimeSeriesType *argument;
        const TimeSeriesReferenceType *type;
        std::unique_ptr<ReferenceNodeTemplate> node_template;
    };

    static auto mutex = std::mutex{};
    static auto templates = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing = std::ranges::find_if(
        templates, [&](const auto &entry) { return entry.argument == &argument && entry.type == &type; });
    if (existing != templates.end()) {
        return *existing->node_template;
    }

    auto &entry = templates.emplace_back(&argument, &type, std::make_unique<ReferenceNodeTemplate>(argument, type));
    return *entry.node_template;
}

WiringPoint materialize_reference(WiringPoint argument, const TimeSeriesType &requested_type) {
    const auto *requested_reference = type_cast<TimeSeriesReferenceType>(&requested_type);
    const auto *argument_reference = type_cast<TimeSeriesReferenceType>(&argument.type());

    if (requested_reference != nullptr && argument_reference == nullptr) {
        if (const auto input = argument.record()->input_binding(argument.type()); input && input->is_borrowed()) {
            throw WiringError{"a borrowed input cannot be materialized as a reference"};
        }
        if (!requested_reference->target_type().accepts(argument.type())) {
            argument = argument.record()->wiring().materialize(argument, requested_reference->target_type());
        }
        if (type_cast<TimeSeriesReferenceType>(&argument.type()) != nullptr) {
            return argument;
        }

        const auto &reference_type =
            time_series_reference_type(requested_reference->target_type(), TimeSeriesGuarantee::const_);
        const auto result = reference_node_template(argument.type(), reference_type)(argument);
        if (!result) {
            throw WiringError{"a reference node produced no output"};
        }
        return *result;
    }

    if (argument_reference == nullptr && requested_type.requires_reference_representation_for(argument.type())) {
        const auto &reference_type = time_series_reference_type(requested_type, TimeSeriesGuarantee::const_);
        const auto result = reference_node_template(argument.type(), reference_type)(argument);
        if (!result) {
            throw WiringError{"a reference node produced no output"};
        }
        return *result;
    }

    return argument;
}

WiringPoint materialize_inherited_reference(WiringPoint argument) {
    const auto *argument_reference = type_cast<TimeSeriesReferenceType>(&argument.type());
    if (argument_reference == nullptr) {
        const auto &reference_type = time_series_reference_type(argument.type(), TimeSeriesGuarantee::const_);
        const auto result = reference_node_template(argument.type(), reference_type)(argument);
        if (!result) {
            throw WiringError{"an inherited reference node produced no output"};
        }
        return *result;
    }
    if (argument_reference->guarantee().value() == TimeSeriesGuarantee::const_) {
        return argument;
    }

    const auto &transport_type = inherited_reference_transport_type(*argument_reference);
    const auto result = reference_node_template(*argument_reference, transport_type)(argument);
    if (!result) {
        throw WiringError{"an inherited reference transport node produced no output"};
    }
    return *result;
}

WiringPoint materialize_late_inherited_reference(WiringPoint argument, const TimeSeriesType &transport_type) {
    if (type_cast<TimeSeriesReferenceType>(&argument.type()) == nullptr) {
        argument = materialize_inherited_reference(argument);
    }

    const auto &reference = type_cast<TimeSeriesReferenceType>(argument.type());
    const auto &transport = type_cast<TimeSeriesReferenceType>(transport_type);
    const auto result = reference_node_template(reference, transport)(argument);
    if (!result) {
        throw WiringError{"a late inherited reference transport node produced no output"};
    }
    return *result;
}

WiringPoint forward_inherited_reference(WiringPoint source, const TimeSeriesReferenceType &type) {
    auto &wiring = source.record()->wiring();
    auto node_template = std::make_unique<RuntimeSignatureNodeTemplate<InheritedReferenceForwarder>>(
        InheritedReferenceForwarder{type});
    const auto &owned = wiring.own_node_template(std::move(node_template));
    const auto result = owned(source);
    if (!result) {
        throw WiringError{"an inherited reference forwarder produced no output"};
    }
    return *result;
}

} // namespace bg::details
