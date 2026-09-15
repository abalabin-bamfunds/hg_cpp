#include <bg/time_series/ref/dynamic_reference_input.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>

namespace bg::details {

bool DynamicReferenceInputStorage::refresh(EvaluationTime evaluation_time) noexcept {
    const auto value = binding_address_ != nullptr ? binding_runtime_type_->resolve_binding(binding_address_)
                                                   : Reference{};
    if (value == value_) {
        return false;
    }

    value_ = value;
    change_time_ = evaluation_time;
    return true;
}

void DynamicReferenceInputStorage::bind(void *address, const TimeSeriesReferenceRuntimeType &runtime_type,
                                        EvaluationContext &context, std::size_t storage_offset) {
    if (address != binding_address_ || &runtime_type != binding_runtime_type_) {
        if (address != nullptr) {
            runtime_type.add_binding_subscriber(address, context.runtime_graph(),
                                                static_cast<std::uint32_t>(storage_offset), storage_offset,
                                                static_cast<std::uint32_t>(context.node_index()), &notify);
        }
        if (binding_address_ != nullptr) {
            binding_runtime_type_->remove_binding_subscriber(binding_address_, context.runtime_graph(),
                                                             storage_offset);
        }
        binding_address_ = address;
        binding_runtime_type_ = &runtime_type;
    }

    refresh(context.evaluation_time());
}

void DynamicReferenceInputStorage::unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept {
    if (binding_address_ != nullptr) {
        binding_runtime_type_->remove_binding_subscriber(binding_address_, graph, storage_offset);
    }

    binding_address_ = nullptr;
    binding_runtime_type_ = nullptr;
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
