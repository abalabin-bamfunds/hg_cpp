#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/graph/runtime_graph_owner.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <type_traits>

namespace bg {

class CompiledGraph;
class EvaluationContext;
class RuntimeGraph;

struct CompiledNestedGraph {
    std::shared_ptr<const CompiledGraph> graph;
    std::pmr::vector<std::size_t> input_nodes;
    std::size_t output_node;
};

struct NestedGraphInput {
    Reference reference;
    EvaluationTime last_updated;
};

// Owns one active inner runtime graph and keeps a stopped instance alive until the next outer cycle.
class NestedGraph {
  public:
    void start(const CompiledNestedGraph &compiled, EvaluationContext &context,
               std::span<const NestedGraphInput> inputs);
    void start(const CompiledNestedGraph &compiled, EvaluationContext &context,
               std::span<const NestedGraphInput> inputs, RuntimeGraphOwner owner);
    void update_inputs(const CompiledNestedGraph &compiled, std::span<const NestedGraphInput> inputs);
    void evaluate(EvaluationContext &context);
    void stop(EvaluationContext &context);
    void stop_and_destroy(EvaluationContext &context) noexcept;

    bool running() const noexcept { return active_ != nullptr; }
    Reference output(const CompiledNestedGraph &compiled) const noexcept;

  private:
    static void destroy_retired_callback(void *receiver) noexcept;
    static void destroy(RuntimeGraph *graph) noexcept;
    void start(const CompiledNestedGraph &compiled, EvaluationContext &context,
               std::span<const NestedGraphInput> inputs, const RuntimeGraphOwner *owner);
    void destroy_retired() noexcept;
    void bind_input(std::size_t node_index, const NestedGraphInput &input);

    RuntimeGraph *active_{};
    RuntimeGraph *retired_{};
};

static_assert(std::is_trivially_copyable_v<NestedGraph>);

} // namespace bg
