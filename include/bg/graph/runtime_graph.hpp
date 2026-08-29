#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace bg {

namespace details {
class EvaluationScratch;
}

class TimeSeriesStorageRepresentation;
class ItemSubscribers;
class SimulationEvaluationEngine;

class RuntimeGraph {
  public:
    explicit RuntimeGraph(const CompiledGraph &compiled_graph);
    RuntimeGraph(const CompiledGraph &compiled_graph, std::pmr::memory_resource &memory);
    RuntimeGraph(const CompiledGraph &compiled_graph, GlobalState &global_state);
    RuntimeGraph(const CompiledGraph &compiled_graph, std::pmr::memory_resource &memory, GlobalState &global_state);
    ~RuntimeGraph();

    RuntimeGraph(const RuntimeGraph &) = delete;
    RuntimeGraph &operator=(const RuntimeGraph &) = delete;
    RuntimeGraph(RuntimeGraph &&) = delete;
    RuntimeGraph &operator=(RuntimeGraph &&) = delete;

    void start(EvaluationTime evaluation_time);
    void stop() noexcept;
    void schedule(std::size_t node_index) noexcept;
    void evaluate(EvaluationTime evaluation_time);
    [[nodiscard]] std::optional<EvaluationTime> next_scheduled_time() const noexcept;
    [[nodiscard]] GlobalState &global_state() const noexcept { return *global_state_; }

  private:
    friend EvaluationContext;
    friend SimulationEvaluationEngine;

    using ScheduleWord = CompiledGraph::ScheduleWord;
    using SchedulingInfo = CompiledGraph::SchedulingInfo;

    enum class OutputChange {
        update,
        invalidation,
        revalidation,
    };

    class ScheduledNodes;
    class FutureSchedule;
    class ScratchUse;

    struct OutputMutation {
        EvaluationContext *context;
        const TimeSeriesStorageRepresentation *representation;
        EvaluationTime *last_updated;
        void *value;
        std::uint32_t item;
        bool published;
        OutputMutation *parent;
        OutputMutation *next;
        const ItemSubscribers *subscribers;
    };

    static_assert(sizeof(OutputMutation) == 64);

    [[nodiscard]] auto scheduling_info() noexcept -> std::span<SchedulingInfo>;
    void initialize_storage();

    void schedule_validity_check(std::size_t node_index) noexcept;
    [[nodiscard]] ScheduleHandle schedule_at(std::size_t node_index, EvaluationTime time);
    void cancel(std::size_t node_index, ScheduleHandle handle) noexcept;
    [[nodiscard]] FutureSchedule *&future_schedule() noexcept;
    [[nodiscard]] FutureSchedule *future_schedule() const noexcept;
    void activate_scheduled(EvaluationTime evaluation_time) noexcept;
    void clear_future_schedule() noexcept;
    [[nodiscard]] OutputMutation *add_output_mutation(EvaluationContext &context,
                                                      const TimeSeriesStorageRepresentation &representation,
                                                      EvaluationTime &last_updated, void *value, std::size_t item,
                                                      OutputMutation *parent);
    void publish_output_mutation(OutputMutation &mutation) noexcept;
    void schedule_output_mutations(const EvaluationContext &context) noexcept;
    void release_output_mutations(EvaluationContext &context) noexcept;
    void schedule_subscribers(std::span<const Subscriber> subscribers, OutputChange change) noexcept;
    void schedule_output_change(const CompiledNode &node, OutputChange change) noexcept;
    void apply_invalidation(const CompiledNode &node, std::size_t node_index);
    void stop_nodes(std::size_t node_count) noexcept;
    [[nodiscard]] bool inputs_are_valid(const CompiledNode &node) const noexcept;
    void start(EvaluationTime evaluation_time, details::EvaluationScratch &scratch);
    void evaluate(EvaluationTime evaluation_time, details::EvaluationScratch &scratch);

    const CompiledGraph *compiled_graph_;
    std::pmr::memory_resource *memory_;
    std::unique_ptr<GlobalState> owned_global_state_;
    GlobalState *global_state_;
    std::byte *storage_{};
    EvaluationTime last_evaluation_time_{MIN_DT};
    EvaluationTime evaluation_time_{MIN_DT};
    details::EvaluationScratch *scratch_{};
};

} // namespace bg
