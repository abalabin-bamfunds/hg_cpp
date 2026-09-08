#include "ref_node.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

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

const NodeTemplate &reference_node_template(const TimeSeriesType &source, const TimeSeriesReferenceType &type) {
    struct Key {
        const TimeSeriesType *source;
        const TimeSeriesReferenceType *type;

        bool operator==(const Key &) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key &key) const noexcept {
            return std::hash<const TimeSeriesType *>{}(key.source) ^
                   (std::hash<const TimeSeriesReferenceType *>{}(key.type) << 1U);
        }
    };

    static auto mutex = std::mutex{};
    static auto templates = std::unordered_map<Key, std::unique_ptr<ReferenceNodeTemplate>, Hash>{};

    const auto lock = std::lock_guard{mutex};
    auto [entry, inserted] = templates.try_emplace(Key{&source, &type});
    if (inserted) {
        entry->second = std::make_unique<ReferenceNodeTemplate>(source, type);
    }
    return *entry->second;
}

} // namespace bg::details
