#pragma once

#include <bg/graph/compiled_node.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/time_series/time_series_input.hpp>

#include <cstddef>
#include <type_traits>

namespace bg {

// Binds one compiled node to its state in a runtime graph.
class Node {
  public:
    struct InputData {
        const TimeSeriesState *last_updated;
        const void *value;
    };

    Node(const CompiledNode &compiled_node, void *runtime_storage) noexcept
        : compiled_node_{&compiled_node}, runtime_storage_{static_cast<std::byte *>(runtime_storage)} {}

    [[nodiscard]] static Node from_output(const CompiledNode &compiled_node, void *output_value) noexcept {
        auto *value = static_cast<std::byte *>(output_value);
        return {compiled_node, value - compiled_node.output_value_offset()};
    }

    std::size_t input_count() const noexcept { return compiled_node_->parameters().size(); }

    DefaultInputState &default_input(std::size_t parameter_index) const noexcept {
        return default_input(compiled_node_->parameters()[parameter_index]);
    }

    const DefaultInputDescriptor &default_descriptor(std::size_t parameter_index) const noexcept {
        return default_descriptor(compiled_node_->parameters()[parameter_index]);
    }

    const TimeSeriesState &input_last_updated(std::size_t parameter_index) const noexcept {
        return *input_data(parameter_index).last_updated;
    }

    bool input_modified(std::size_t parameter_index, EvaluationTime evaluation_time) const noexcept {
        const auto &state = input_last_updated(parameter_index);
        return state.valid() && state.time() == evaluation_time;
    }

    InputData input_data(std::size_t parameter_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        const auto mode = binding.mode();
        if (mode == ParameterBindingMode::default_value) {
            const auto &descriptor = default_descriptor(binding);
            const auto &source = descriptor.source;
            const auto source_data = binding_data(source, source.mode());
            const auto *last_updated = &default_input(binding).last_updated;
            if (!last_updated->valid()) {
                last_updated =
                    reinterpret_cast<const TimeSeriesState *>(runtime_storage_ + descriptor.start_time_offset);
            }
            if (!source_data.last_updated->valid()) {
                return {last_updated, descriptor.fallback};
            }

            return {last_updated, source_data.value};
        }

        return binding_data(binding, mode);
    }

    const void *input_value(std::size_t parameter_index) const noexcept { return input_data(parameter_index).value; }

    const TimeSeriesRuntimeType &time_series_input_runtime_type(std::size_t parameter_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        if (binding.mode() == ParameterBindingMode::default_value) {
            const auto &descriptor = default_descriptor(binding);
            if (!binding_data(descriptor.source, descriptor.source.mode()).last_updated->valid()) {
                return compiled_node_->time_series_input_runtime_type(parameter_index);
            }
        }
        if (const auto *runtime_type = bound_reference_runtime_type(parameter_index)) {
            return *runtime_type;
        }

        return compiled_node_->time_series_input_runtime_type(parameter_index);
    }

    const TimeSeriesRuntimeType &all_valid_input_runtime_type(
        const AllValidInputDescriptor &descriptor) const noexcept {
        if (!descriptor.runtime_type->is_reference()) {
            if (const auto *runtime_type = bound_reference_runtime_type(descriptor.parameter_index)) {
                return *runtime_type;
            }
        }

        return *descriptor.runtime_type;
    }

    const TimeSeriesRuntimeType *bound_reference_runtime_type(std::size_t parameter_index) const noexcept {
        if (const auto *sample = reference_input_sample(parameter_index); sample != nullptr && !sample->empty()) {
            return &sample->input().runtime_type;
        }
        const auto *input = reference_input(parameter_index);
        if (input == nullptr || input->value == nullptr) {
            return nullptr;
        }

        return details::ResolvedTimeSeries::resolve(input->value).runtime_type;
    }

    In<TS_T> time_series_input(std::size_t parameter_index, EvaluationTime evaluation_time = MIN_DT) const noexcept {
        const auto input = input_data(parameter_index);
        return {*input.last_updated, input.value, time_series_input_runtime_type(parameter_index),
                input.last_updated->valid() && input.last_updated->time() == evaluation_time};
    }

    bool *input_activity(std::size_t controlled_input_index) const noexcept {
        const auto inputs = compiled_node_->controlled_inputs();
        return reinterpret_cast<bool *>(runtime_storage_ + inputs[controlled_input_index].activity_offset());
    }

    ReferenceInputStorage *reference_input(std::size_t parameter_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        const auto *source = &binding;
        auto mode = binding.mode();
        if (mode == ParameterBindingMode::default_value) {
            source = &default_descriptor(binding).source;
            mode = source->mode();
        }
        if (mode != ParameterBindingMode::reference) {
            return nullptr;
        }
        return reinterpret_cast<ReferenceInputStorage *>(runtime_storage_ + source->value_offset());
    }

    details::ReferenceInputSampleStorage *reference_input_sample(std::size_t parameter_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        const auto *source = &binding;
        if (binding.mode() == ParameterBindingMode::default_value) {
            source = &default_descriptor(binding).source;
        }
        return reference_input_sample(source);
    }

    DefaultInputState &default_input(const ParameterBinding &binding) const noexcept {
        const auto offset = binding.last_updated_offset();
        return *reinterpret_cast<DefaultInputState *>(runtime_storage_ + offset);
    }

    const DefaultInputDescriptor &default_descriptor(const ParameterBinding &binding) const noexcept {
        const auto *address = reinterpret_cast<const std::byte *>(&binding) + binding.value_offset();
        return *reinterpret_cast<const DefaultInputDescriptor *>(address);
    }

    TimeSeriesState &output_last_updated() const noexcept {
        return *reinterpret_cast<TimeSeriesState *>(runtime_storage_ + compiled_node_->output_last_updated_offset());
    }

    void *output_value() const noexcept { return runtime_storage_ + compiled_node_->output_value_offset(); }

    void *state() const noexcept { return runtime_storage_ + compiled_node_->state_offset(); }

  private:
    InputData binding_data(const ParameterBinding &binding, ParameterBindingMode mode) const noexcept {
        if (mode == ParameterBindingMode::time_series) {
            const auto &last_updated =
                *reinterpret_cast<const TimeSeriesState *>(runtime_storage_ + binding.last_updated_offset());
            return {&last_updated, runtime_storage_ + binding.value_offset()};
        }

        if (mode == ParameterBindingMode::runtime_input) {
            const auto &source =
                *reinterpret_cast<const RuntimeInputBinding *>(runtime_storage_ + binding.value_offset());
            return {&source.last_updated, source.value};
        }

        const auto *wired_last_updated =
            reinterpret_cast<const TimeSeriesState *>(runtime_storage_ + binding.last_updated_offset());
        if (mode == ParameterBindingMode::constant) {
            return {wired_last_updated, binding.constant_data()};
        }
        if (mode != ParameterBindingMode::reference) {
            return {wired_last_updated, runtime_storage_ + binding.value_offset()};
        }

        if (const auto *sample = reference_input_sample(&binding); sample != nullptr && !sample->empty()) {
            const auto input = sample->input();
            return {&input.last_updated, input.value};
        }

        const auto reference =
            reinterpret_cast<const ReferenceInputStorage *>(runtime_storage_ + binding.value_offset())->value;
        if (!reference) {
            return {wired_last_updated->valid() ? &initial_time_series_state : wired_last_updated, nullptr};
        }

        const auto resolved = details::ResolvedTimeSeries::resolve(reference);
        if (!resolved.last_updated->valid() || !wired_last_updated->later_than(*resolved.last_updated)) {
            return {resolved.last_updated, resolved.storage};
        }

        return {wired_last_updated, resolved.storage};
    }

    details::ReferenceInputSampleStorage *reference_input_sample(const ParameterBinding *binding) const noexcept {
        if (!binding->has_reference_sample()) {
            return nullptr;
        }
        const auto offset = details::ReferenceInputSampleStorage::offset_for(binding->value_offset(),
                                                                            binding->reference_dynamic_activity());
        return reinterpret_cast<details::ReferenceInputSampleStorage *>(runtime_storage_ + offset);
    }

    const CompiledNode *compiled_node_;
    std::byte *runtime_storage_;
};

static_assert(sizeof(Node) == 2 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<Node>);

} // namespace bg
