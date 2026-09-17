#pragma once

#include <bg/graph/compiled_node.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_input.hpp>

#include <cstddef>
#include <type_traits>

namespace bg {

// Binds one compiled node to its state in a runtime graph.
class Node {
  public:
    Node(const CompiledNode &compiled_node, void *runtime_storage) noexcept
        : compiled_node_{&compiled_node}, runtime_storage_{static_cast<std::byte *>(runtime_storage)} {}

    [[nodiscard]] static Node from_output(const CompiledNode &compiled_node, void *output_value) noexcept {
        auto *value = static_cast<std::byte *>(output_value);
        return {compiled_node, value - compiled_node.output_value_offset()};
    }

    std::size_t input_count() const noexcept { return compiled_node_->parameters().size(); }

    const EvaluationTime &input_last_updated(std::size_t parameter_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        const auto &wired_last_updated =
            *reinterpret_cast<const EvaluationTime *>(runtime_storage_ + binding.last_updated_offset());
        if (binding.mode() == ParameterBindingMode::constant) {
            return wired_last_updated;
        }
        if (binding.mode() != ParameterBindingMode::reference) {
            return wired_last_updated;
        }

        const auto reference = reference_input(parameter_index).value;
        if (!reference) {
            return MIN_DT;
        }

        const auto resolved = details::ResolvedTimeSeries::resolve(reference);
        const auto &referenced_last_updated = *resolved.last_updated;
        if (referenced_last_updated <= MIN_DT) {
            return referenced_last_updated;
        }

        return wired_last_updated > referenced_last_updated ? wired_last_updated : referenced_last_updated;
    }

    bool input_modified(std::size_t parameter_index, EvaluationTime evaluation_time) const noexcept {
        return evaluation_time > MIN_DT && input_last_updated(parameter_index) == evaluation_time;
    }

    const void *input_value(std::size_t parameter_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        if (binding.mode() == ParameterBindingMode::constant) {
            return binding.constant_data();
        }
        if (binding.mode() != ParameterBindingMode::reference) {
            return runtime_storage_ + binding.value_offset();
        }

        const auto reference = reference_input(parameter_index).value;
        return reference ? details::ResolvedTimeSeries::resolve(reference).storage : nullptr;
    }

    const TimeSeriesRuntimeType &time_series_input_runtime_type(std::size_t parameter_index,
                                                                std::size_t runtime_type_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        if (binding.mode() == ParameterBindingMode::reference) {
            const auto reference = reference_input(parameter_index).value;
            if (reference) {
                if (const auto *runtime_type = details::ResolvedTimeSeries::resolve(reference).runtime_type) {
                    return *runtime_type;
                }
            }
        }

        return compiled_node_->time_series_input_runtime_type(runtime_type_index);
    }

    In<TS_T> time_series_input(std::size_t parameter_index, std::size_t runtime_type_index,
                               EvaluationTime evaluation_time = MIN_DT) const noexcept {
        return {input_last_updated(parameter_index), input_value(parameter_index),
                time_series_input_runtime_type(parameter_index, runtime_type_index),
                input_modified(parameter_index, evaluation_time)};
    }

    ReferenceInputStorage &reference_input(std::size_t parameter_index) const noexcept {
        const auto offset = compiled_node_->parameters()[parameter_index].value_offset();
        return *reinterpret_cast<ReferenceInputStorage *>(runtime_storage_ + offset);
    }

    EvaluationTime &output_last_updated() const noexcept {
        return *reinterpret_cast<EvaluationTime *>(runtime_storage_ + compiled_node_->output_last_updated_offset());
    }

    void *output_value() const noexcept {
        return runtime_storage_ + compiled_node_->output_value_offset();
    }

    void *state() const noexcept { return runtime_storage_ + compiled_node_->state_offset(); }

  private:
    const CompiledNode *compiled_node_;
    std::byte *runtime_storage_;
};

static_assert(sizeof(Node) == 2 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<Node>);

} // namespace bg
