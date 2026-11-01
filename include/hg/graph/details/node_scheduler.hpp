#pragma once

#include <cstddef>

namespace bg {

class EvaluationContext;
class RuntimeGraph;
struct RuntimeGraphInput;

namespace details {

class NodeScheduler {
  public:
    NodeScheduler() noexcept = default;
    explicit NodeScheduler(const EvaluationContext &context) noexcept;

    explicit operator bool() const noexcept { return graph_ != nullptr; }
    void schedule() const noexcept;
    RuntimeGraph &runtime_graph() const noexcept { return *graph_; }
    std::size_t node_index() const noexcept { return node_index_; }
    EvaluationContext node_context(EvaluationContext &parent, const RuntimeGraphInput *inputs) const noexcept;

  private:
    RuntimeGraph *graph_{};
    std::size_t node_index_{};
};

static_assert(sizeof(NodeScheduler) == 2 * sizeof(void *));

} // namespace details
} // namespace bg
