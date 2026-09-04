#pragma once

#include <bg/graph/compiled_node.hpp>
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

    [[nodiscard]] std::size_t input_count() const noexcept { return compiled_node_->parameters().size(); }

    [[nodiscard]] const EvaluationTime &input_last_updated(std::size_t parameter_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        const auto &source_last_updated =
            *reinterpret_cast<const EvaluationTime *>(runtime_storage_ + binding.last_updated_offset());
        if (binding.mode() != ParameterBindingMode::reference) {
            return source_last_updated;
        }

        const auto *target = reference_input(parameter_index).target;
        if (target == nullptr) {
            return MIN_DT;
        }

        const auto &target_last_updated = *target->target_last_updated;
        if (target_last_updated <= MIN_DT) {
            return target_last_updated;
        }

        return source_last_updated > target_last_updated ? source_last_updated : target_last_updated;
    }

    [[nodiscard]] const void *input_value(std::size_t parameter_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        if (binding.mode() != ParameterBindingMode::reference) {
            return runtime_storage_ + binding.value_offset();
        }

        const auto *target = reference_input(parameter_index).target;
        return target == nullptr ? nullptr : target->target_value;
    }

    [[nodiscard]] const TimeSeriesRuntimeType &time_series_input_runtime_type(
        std::size_t parameter_index, std::size_t runtime_type_index) const noexcept {
        const auto &binding = compiled_node_->parameters()[parameter_index];
        if (binding.mode() == ParameterBindingMode::reference) {
            const auto *target = reference_input(parameter_index).target;
            if (target != nullptr) {
                return *target->target_runtime_type;
            }
        }

        return compiled_node_->time_series_input_runtime_type(runtime_type_index);
    }

    [[nodiscard]] In<TS_T> time_series_input(std::size_t parameter_index,
                                              std::size_t runtime_type_index) const noexcept {
        return {input_last_updated(parameter_index), input_value(parameter_index),
                time_series_input_runtime_type(parameter_index, runtime_type_index)};
    }

    [[nodiscard]] details::ReferenceInputStorage &reference_input(std::size_t parameter_index) const noexcept {
        const auto offset = compiled_node_->parameters()[parameter_index].value_offset();
        return *reinterpret_cast<details::ReferenceInputStorage *>(runtime_storage_ + offset);
    }

    [[nodiscard]] EvaluationTime &output_last_updated() const noexcept {
        return *reinterpret_cast<EvaluationTime *>(runtime_storage_ + compiled_node_->output_last_updated_offset());
    }

    [[nodiscard]] void *output_value() const noexcept {
        return runtime_storage_ + compiled_node_->output_value_offset();
    }

    [[nodiscard]] void *state() const noexcept { return runtime_storage_ + compiled_node_->state_offset(); }

  private:
    const CompiledNode *compiled_node_;
    std::byte *runtime_storage_;
};

static_assert(sizeof(Node) == 2 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<Node>);

} // namespace bg
