#include "ref_node.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/node/node_template.hpp>
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

    const Signature &signature() const override { return signature_; }

    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return nullptr; }

    EvaluationFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }

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

    EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept override {
        return &destroy_state;
    }

    bool uses_time_series_input_runtime_type(std::size_t) const noexcept override { return true; }

    bool uses_cycle_callbacks() const noexcept override { return uses_cycle_callbacks_; }

    InputChangeCallback input_change_callback(std::size_t) const noexcept override { return {&notify_input, 0}; }

  private:
    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        const auto runtime_node = Node{node, runtime_storage};
        auto &root = *static_cast<RootReference *>(runtime_node.state());
        auto &output = *static_cast<Reference *>(runtime_node.output_value());

        root.owner_graph = &context->runtime_graph();
        root.reference_node = &node;
        root.referenced_runtime_type = &runtime_node.time_series_input_runtime_type(0, 0);
        output = &root;
        runtime_node.output_last_updated() = context->evaluation_time();
    }

    static void destroy_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        auto &root = *static_cast<RootReference *>(Node{node, runtime_storage}.state());
        destroy_root_reference(root, context->runtime_graph().memory_resource());
    }

    static void notify_input(OutputChangeContext &context, std::size_t node_index, std::size_t, OutputChange change) {
        auto &root = *static_cast<RootReference *>(context.runtime_graph().node(node_index).state());
        notify_reference(Reference{&root}, context, change);
    }

    Signature signature_;
    bool uses_cycle_callbacks_;
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
    const auto *requested_reference = dynamic_cast<const TimeSeriesReferenceType *>(&requested_type);
    const auto *argument_reference = dynamic_cast<const TimeSeriesReferenceType *>(&argument.type());

    if (requested_reference != nullptr && argument_reference == nullptr) {
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

} // namespace bg::details
