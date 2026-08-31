#include "ref_node.hpp"

#include <bg/graph/compiled_node.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/ref/ref_storage_representation.hpp>
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
        return &evaluate;
    }

    [[nodiscard]] EvaluationFunction start_function(const ResolvedSignature &) const noexcept override {
        return &start;
    }

    [[nodiscard]] NodeStateLayout state_layout(const ResolvedSignature &,
                                               std::span<const ScalarStorageRepresentation *const>,
                                               std::span<const ScalarType *const>) const override {
        return {
            .size = sizeof(ReferenceStorage),
            .alignment = alignof(ReferenceStorage),
            .field_offsets = {},
            .generic_storage_representations = {},
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

    [[nodiscard]] const TimeSeriesStorageRepresentation *output_storage_representation(
        const ResolvedSignature &signature, std::span<const ScalarStorageRepresentation *const>,
        std::span<const TimeSeriesStorageRepresentation *const> inputs) const noexcept override {
        return &time_series_reference_storage_representation(
            static_cast<const TimeSeriesReferenceType &>(*signature.output), *inputs.front());
    }

  private:
    static void evaluate(const CompiledNode &, void *, EvaluationContext *) {}

    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        const auto &target = node.parameters().front();
        auto *storage = static_cast<std::byte *>(runtime_storage);
        auto &reference = *reinterpret_cast<ReferenceStorage *>(storage + node.state_offset());
        auto &output = *reinterpret_cast<ReferenceStorage **>(storage + node.output_value_offset());

        reference.owner = &context->runtime_graph();
        reference.target_last_updated = &target.last_updated(runtime_storage);
        reference.target_value = target.value(runtime_storage);
        output = &reference;
        node.output_last_updated(runtime_storage) = context->evaluation_time();
    }

    static void destroy_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *) noexcept {
        auto *storage = static_cast<std::byte *>(runtime_storage);
        auto &reference = *reinterpret_cast<ReferenceStorage *>(storage + node.state_offset());
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
