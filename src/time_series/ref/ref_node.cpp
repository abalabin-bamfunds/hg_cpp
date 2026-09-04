#include "ref_node.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/node.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg::details {
namespace {

class ReferenceNodeTemplate final : public NodeTemplate {
  public:
    explicit ReferenceNodeTemplate(const TimeSeriesReferenceType &type)
        : signature_{
              util::symbol{"reference"}, {{util::symbol{"target"}, &type.target_type(), passive, optional}}, type} {}

    [[nodiscard]] const Signature &signature() const override { return signature_; }

    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return nullptr;
    }

    [[nodiscard]] EvaluationFunction start_function(const ResolvedSignature &) const noexcept override {
        return &start;
    }

    [[nodiscard]] NodeStateLayout state_layout(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                                               std::span<const ScalarType *const>) const override {
        return {
            .size = sizeof(ReferenceStorage),
            .alignment = alignof(ReferenceStorage),
            .field_offsets = {},
            .generic_runtime_types = {},
        };
    }

    void construct_initial_state(const NodeStateLayout &, void *storage) const noexcept override {
        std::construct_at(static_cast<ReferenceStorage *>(storage));
    }

    [[nodiscard]] EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept override {
        return &destroy_state;
    }

    [[nodiscard]] std::optional<DynamicSubscribersLayout>
    dynamic_subscribers_layout(const ResolvedSignature &) const noexcept override {
        return DynamicSubscribersLayout{.source_input = 0, .state_offset = offsetof(ReferenceStorage, subscribers)};
    }

    [[nodiscard]] bool uses_time_series_input_runtime_type(std::size_t) const noexcept override { return true; }

    [[nodiscard]] const TimeSeriesRuntimeType *
    output_runtime_type(const ResolvedSignature &signature, std::span<const ScalarRuntimeType *const>,
                        std::span<const TimeSeriesRuntimeType *const> inputs) const noexcept override {
        return &time_series_reference_runtime_type(static_cast<const TimeSeriesReferenceType &>(*signature.output),
                                                   *inputs.front());
    }

  private:
    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        const auto runtime_node = Node{node, runtime_storage};
        auto &reference = *static_cast<ReferenceStorage *>(runtime_node.state());
        auto &output = *static_cast<ReferenceStorage **>(runtime_node.output_value());

        reference.owner = &context->runtime_graph();
        reference.target_last_updated = &runtime_node.input_last_updated(0);
        reference.target_value = runtime_node.input_value(0);
        reference.target_runtime_type = &runtime_node.time_series_input_runtime_type(0, 0);
        output = &reference;
        runtime_node.output_last_updated() = context->evaluation_time();
    }

    static void destroy_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *) noexcept {
        auto &reference = *static_cast<ReferenceStorage *>(Node{node, runtime_storage}.state());
        DynamicSubscribers::release(reference.subscribers);
        std::destroy_at(&reference);
    }

    Signature signature_;
};

} // namespace

const NodeTemplate &reference_node_template(const TimeSeriesReferenceType &type) {
    static auto mutex = std::mutex{};
    static auto templates =
        std::unordered_map<const TimeSeriesReferenceType *, std::unique_ptr<ReferenceNodeTemplate>>{};

    const auto lock = std::lock_guard{mutex};
    auto [entry, inserted] = templates.try_emplace(&type);
    if (inserted) {
        entry->second = std::make_unique<ReferenceNodeTemplate>(type);
    }
    return *entry->second;
}

} // namespace bg::details
