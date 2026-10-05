#pragma once

#include <bg/scalar/scalar.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <type_traits>

namespace bg {

class CompiledGraph;
class EvaluationContext;
class RuntimeGraph;
class RuntimeGraphEvaluator;

struct CompiledNestedGraph {
    std::shared_ptr<const CompiledGraph> graph;
    std::pmr::vector<std::size_t> input_nodes;
    std::pmr::vector<std::size_t> constant_input_nodes;
    std::pmr::vector<std::uint32_t> parent_service_binding_indices;
    std::optional<std::size_t> output_node;
};

struct NestedGraphInput {
    Reference reference;
};

struct NestedGraphConstantInput {
    Scalar<> value;
};

// Owns one active inner runtime graph and keeps a stopped instance alive until the next outer cycle.
class NestedGraph {
  public:
    void start(const CompiledNestedGraph &compiled, RuntimeGraphEvaluator &evaluator, EvaluationContext &context,
               std::span<const NestedGraphInput> inputs,
               std::size_t child_token = 0);
    void start(const CompiledNestedGraph &compiled, RuntimeGraphEvaluator &evaluator, EvaluationContext &context,
               std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants,
               std::size_t child_token = 0);
    void update_inputs(const CompiledNestedGraph &compiled, std::span<const NestedGraphInput> inputs,
                       EvaluationContext &context);
    void update_input(const CompiledNestedGraph &compiled, std::size_t input_index, NestedGraphInput input,
                      EvaluationContext &context);
    bool evaluate(EvaluationContext &context);
    void stop(EvaluationContext &context);
    void stop_and_destroy(EvaluationContext &context) noexcept;

    bool running() const noexcept { return active_ != nullptr; }
    bool evaluation_pending() const noexcept;
    Reference output(const CompiledNestedGraph &compiled) const noexcept;

  private:
    static void destroy_retired_callback(void *receiver, EvaluationContext &context) noexcept;
    static void destroy(RuntimeGraph *graph, EvaluationContext &context) noexcept;
    void destroy_retired(EvaluationContext &context) noexcept;

    RuntimeGraph *active_{};
    RuntimeGraph *retired_{};
};

static_assert(std::is_trivially_copyable_v<NestedGraph>);

} // namespace bg
