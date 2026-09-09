#include "ref_node.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace bg::details {
namespace {

class ReferenceNodeTemplate final : public NodeTemplate {
  public:
    ReferenceNodeTemplate(const TimeSeriesType &source, const TimeSeriesReferenceType &type)
        : signature_{util::symbol{"reference"}, {{util::symbol{"target"}, &source, passive, optional}}, type} {}

    const Signature &signature() const override { return signature_; }

    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return nullptr; }

    EvaluationFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }

    NodeStateLayout state_layout(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                                 std::span<const ScalarType *const>) const override {
        return {
            .size = sizeof(ReferenceRoot),
            .alignment = alignof(ReferenceRoot),
            .field_offsets = {},
            .generic_runtime_types = {},
        };
    }

    void construct_initial_state(const NodeStateLayout &, void *storage) const noexcept override {
        std::construct_at(static_cast<ReferenceRoot *>(storage));
    }

    EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept override {
        return &destroy_state;
    }

    bool uses_time_series_input_runtime_type(std::size_t) const noexcept override { return true; }

    InputChangeCallback input_change_callback(std::size_t) const noexcept override { return {&source_changed, 0}; }

    const TimeSeriesRuntimeType *
    output_runtime_type(const ResolvedSignature &signature, std::span<const ScalarRuntimeType *const>,
                        std::span<const TimeSeriesRuntimeType *const> inputs) const noexcept override {
        return &time_series_reference_runtime_type(static_cast<const TimeSeriesReferenceType &>(*signature.output),
                                                   *inputs.front());
    }

  private:
    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        const auto runtime_node = Node{node, runtime_storage};
        auto &reference = *static_cast<ReferenceRoot *>(runtime_node.state());
        auto &output = *static_cast<ReferenceAddress *>(runtime_node.output_value());

        reference.graph = &context->runtime_graph();
        reference.source = &node;
        reference.runtime_type = &runtime_node.time_series_input_runtime_type(0, 0);
        output = &reference;
        runtime_node.output_last_updated() = context->evaluation_time();
    }

    static void destroy_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        auto &reference = *static_cast<ReferenceRoot *>(Node{node, runtime_storage}.state());
        destroy_reference_root(reference, context->runtime_graph().memory_resource());
    }

    static void source_changed(OutputChangeContext &context, std::size_t node_index, std::size_t, OutputChange change) {
        auto &reference = *static_cast<ReferenceRoot *>(context.runtime_graph().node(node_index).state());
        notify_reference_source(reference, context, change);
    }

    Signature signature_;
};

} // namespace

bool requires_reference_representation(const TimeSeriesType &parameter, const TimeSeriesType &argument) noexcept {
    const auto *parameter_dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&parameter);
    const auto *argument_dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&argument);
    if (parameter_dictionary == nullptr || argument_dictionary == nullptr) {
        return false;
    }

    const auto &parameter_item = parameter_dictionary->value_type();
    const auto &argument_item = argument_dictionary->value_type();
    const auto parameter_is_reference = dynamic_cast<const TimeSeriesReferenceType *>(&parameter_item) != nullptr;
    const auto argument_is_reference = dynamic_cast<const TimeSeriesReferenceType *>(&argument_item) != nullptr;
    if (parameter_is_reference != argument_is_reference) {
        return true;
    }

    return requires_reference_representation(parameter_item, argument_item);
}

const NodeTemplate &reference_node_template(const TimeSeriesType &source, const TimeSeriesReferenceType &type) {
    struct Entry {
        const TimeSeriesType *source;
        const TimeSeriesReferenceType *type;
        std::unique_ptr<ReferenceNodeTemplate> node_template;
    };

    static auto mutex = std::mutex{};
    static auto templates = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing = std::ranges::find_if(templates, [&](const auto &entry) {
        return entry.source == &source && entry.type == &type;
    });
    if (existing != templates.end()) {
        return *existing->node_template;
    }

    auto &entry = templates.emplace_back(&source, &type, std::make_unique<ReferenceNodeTemplate>(source, type));
    return *entry.node_template;
}

WiringPoint materialize_reference(WiringPoint source, const TimeSeriesType &requested_type) {
    const auto *requested_reference = dynamic_cast<const TimeSeriesReferenceType *>(&requested_type);
    const auto *source_reference = dynamic_cast<const TimeSeriesReferenceType *>(&source.type());

    if (requested_reference != nullptr && source_reference == nullptr) {
        const auto result = reference_node_template(source.type(), *requested_reference)(source);
        if (!result) {
            throw WiringError{"a reference node produced no output"};
        }
        return *result;
    }

    if (source_reference == nullptr && requires_reference_representation(requested_type, source.type())) {
        const auto &reference_type = time_series_reference_type(requested_type, TimeSeriesGuarantee::const_);
        const auto result = reference_node_template(source.type(), reference_type)(source);
        if (!result) {
            throw WiringError{"a reference node produced no output"};
        }
        return *result;
    }

    return source;
}

} // namespace bg::details
