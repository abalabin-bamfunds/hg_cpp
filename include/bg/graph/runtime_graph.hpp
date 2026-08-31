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
    class ModifiedItemScheduler;

    struct OutputPath {
        OutputPath(EvaluationContext &context, const TimeSeriesStorageRepresentation &representation,
                   EvaluationTime &last_updated, void *value, std::uint32_t item, OutputPath *parent) noexcept
            : owner{parent == nullptr ? reinterpret_cast<std::uintptr_t>(&context) | context_tag
                                      : reinterpret_cast<std::uintptr_t>(parent)},
              representation{&representation}, last_updated{&last_updated}, value{value}, item{item} {}

        [[nodiscard]] EvaluationContext &context() const noexcept {
            if ((owner & context_tag) != 0) {
                return *reinterpret_cast<EvaluationContext *>(owner & ~context_tag);
            }
            return reinterpret_cast<OutputPath *>(owner)->context();
        }

        [[nodiscard]] OutputPath *parent() const noexcept {
            return (owner & context_tag) == 0 ? reinterpret_cast<OutputPath *>(owner) : nullptr;
        }

        static constexpr auto context_tag = std::uintptr_t{1};

        std::uintptr_t owner;
        const TimeSeriesStorageRepresentation *representation;
        EvaluationTime *last_updated;
        void *value;
        std::uint32_t item;
    };

    static_assert(sizeof(OutputPath) == 40);
    static_assert(alignof(EvaluationContext) >= 2);

    [[nodiscard]] auto scheduling_info() noexcept -> std::span<SchedulingInfo>;
    void initialize_storage();

    void schedule_validity_check(std::size_t node_index) noexcept;
    [[nodiscard]] ScheduleHandle schedule_at(std::size_t node_index, EvaluationTime time);
    void cancel(std::size_t node_index, ScheduleHandle handle) noexcept;
    [[nodiscard]] FutureSchedule *&future_schedule() noexcept;
    [[nodiscard]] FutureSchedule *future_schedule() const noexcept;
    void activate_scheduled(EvaluationTime evaluation_time) noexcept;
    void clear_future_schedule() noexcept;
    [[nodiscard]] OutputPath *add_output_path(EvaluationContext &context,
                                              const TimeSeriesStorageRepresentation &representation,
                                              EvaluationTime &last_updated, void *value, std::size_t item,
                                              OutputPath *parent);
    void ensure_output_frame(EvaluationContext &context);
    [[nodiscard]] TimeSeriesOutputView &add_output_view(EvaluationContext &context,
                                                        const TimeSeriesOutputView &prototype);
    void publish_output_path(OutputPath &path) noexcept;
    void release_output_view(EvaluationContext &context, TimeSeriesOutputView &view) noexcept;
    void release_output_path(EvaluationContext &context, OutputPath &path) noexcept;
    void release_output_paths(EvaluationContext &context) noexcept;
    void schedule_modified_items(const CompiledNode &node);
    void schedule_subscribers(std::span<const Subscriber> subscribers, OutputChange change);
    void schedule_node_subscriber(std::size_t node_index, bool active, bool required, bool active_optional,
                                  OutputChange change) noexcept;
    void schedule_output_change(const CompiledNode &node, OutputChange change);
    void refresh_reference_inputs(std::span<const ReferenceInputSubscriber> subscribers, const void *changed_value,
                                  OutputChange change);
    void schedule_dynamic_subscribers(std::size_t offset, OutputChange change) noexcept;
    void add_dynamic_subscriber(details::DynamicSubscribers *&subscribers, RuntimeGraph &destination,
                                std::size_t node_index, std::size_t parameter_index, bool active, bool required);
    void remove_dynamic_subscriber(details::DynamicSubscribers *subscribers, RuntimeGraph &destination,
                                   std::size_t node_index, std::size_t parameter_index) noexcept;
    void apply_invalidation(const CompiledNode &node, std::size_t node_index);
    void stop_nodes(std::size_t node_count) noexcept;
    [[nodiscard]] bool inputs_are_valid(const CompiledNode &node) const noexcept;
    void start(EvaluationTime evaluation_time, details::EvaluationScratch &scratch);
    void evaluate(EvaluationTime evaluation_time, details::EvaluationScratch &scratch);
    void stop(details::EvaluationScratch &scratch) noexcept;

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
