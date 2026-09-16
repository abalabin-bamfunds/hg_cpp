#include <bg/time_series/ref/dynamic_reference_input.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/graph/runtime_graph.hpp>

namespace bg::details {

bool DynamicReferenceInputStorage::refresh(EvaluationTime evaluation_time) noexcept {
    const auto value = binding_ ? binding_.value() : Reference{};
    if (value == value_) {
        return false;
    }

    value_ = value;
    change_time_ = evaluation_time;
    return true;
}

void DynamicReferenceInputStorage::bind(ReferenceBinding binding, EvaluationContext &context,
                                        std::size_t storage_offset) {
    if (binding != binding_) {
        if (binding_) {
            binding_.unsubscribe(context.runtime_graph(), storage_offset);
        }

        binding_ = binding;
        binding_.subscribe(context.runtime_graph(), static_cast<std::uint32_t>(storage_offset), storage_offset,
                           static_cast<std::uint32_t>(context.node_index()), &notify);
    }

    refresh(context.evaluation_time());
}

void DynamicReferenceInputStorage::unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept {
    if (binding_) {
        binding_.unsubscribe(graph, storage_offset);
    }

    binding_ = {};
    value_ = {};
    change_time_ = MIN_DT;
}

void DynamicReferenceInputStorage::notify(OutputChangeContext &context, std::size_t node_index,
                                          std::size_t storage_offset, OutputChange) {
    auto &input = *static_cast<DynamicReferenceInputStorage *>(
        context.runtime_graph().storage_at(storage_offset));
    if (input.refresh(context.evaluation_time())) {
        context.schedule(context.runtime_graph(), node_index);
    }
}

} // namespace bg::details
