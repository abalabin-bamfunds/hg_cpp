#include <bg/time_series/ref/dynamic_reference_input.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>

namespace bg::details {

bool DynamicReferenceInputStorage::refresh(EvaluationTime evaluation_time) noexcept {
    const auto value = selected_ ? binding_runtime_type_->resolve_binding(selected_) : Reference{};
    if (value == value_) {
        return false;
    }

    value_ = value;
    last_updated_ = value ? evaluation_time : MIN_DT;
    change_time_ = evaluation_time;
    return true;
}

void DynamicReferenceInputStorage::bind(Reference reference, const TimeSeriesReferenceRuntimeType &runtime_type,
                                        EvaluationContext &context, std::size_t storage_offset) {
    if (reference != selected_) {
        if (reference) {
            add_reference_subscriber(reference, context.runtime_graph(), static_cast<std::uint32_t>(storage_offset),
                                     storage_offset, static_cast<std::uint32_t>(context.node_index()), false, false,
                                     &notify);
        }
        if (selected_) {
            remove_reference_subscriber(selected_, context.runtime_graph(), storage_offset);
        }
        selected_ = reference;
    }
    binding_runtime_type_ = &runtime_type;

    refresh(context.evaluation_time());
}

void DynamicReferenceInputStorage::unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept {
    if (selected_) {
        remove_reference_subscriber(selected_, graph, storage_offset);
    }

    selected_ = {};
    binding_runtime_type_ = nullptr;
    value_ = {};
    last_updated_ = MIN_DT;
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
