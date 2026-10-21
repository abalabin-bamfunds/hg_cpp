#include <bg/time_series/dynamic_value_input.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/runtime_graph.hpp>

namespace bg::details {

void DynamicValueInputStorage::bind_raw(void *data, const DynamicValueBindingType *type, EvaluationContext &context,
                                        std::size_t storage_offset) {
    if (data == data_ && type == type_) {
        return;
    }

    if (data != nullptr) {
        type->subscribe(data, context.runtime_graph(), storage_offset,
                        static_cast<std::uint32_t>(context.node_index()));
    }

    if (data_ != nullptr) {
        type_->unsubscribe(data_, context.runtime_graph(), storage_offset);
    }
    data_ = data;
    type_ = type;
}

void DynamicValueInputStorage::unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept {
    if (data_ != nullptr) {
        type_->unsubscribe(data_, graph, storage_offset);
    }
    data_ = nullptr;
    type_ = nullptr;
}

TimeSeriesInputData DynamicValueInputStorage::input_data(const TimeSeriesRuntimeType &runtime_type,
                                                         EvaluationTime evaluation_time) const noexcept {
    return data_ == nullptr ? TimeSeriesInputData{runtime_type, initial_time_series_state, nullptr}
                            : type_->input_data(data_, evaluation_time);
}

} // namespace bg::details
