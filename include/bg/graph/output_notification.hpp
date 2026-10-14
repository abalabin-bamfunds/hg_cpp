#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/graph/runtime_graph_input.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bg {

class EvaluationContext;
class RuntimeGraph;
class TimeSeriesRuntimeType;
namespace details {
struct DynamicSubscribers;
} // namespace details

enum class OutputChange : std::uint8_t {
    update,
    invalidation,
    revalidation,
};

class OutputChangeContext {
  public:
    explicit OutputChangeContext(RuntimeGraph &runtime_graph) noexcept;
    OutputChangeContext(RuntimeGraph &runtime_graph, EvaluationTime evaluation_time) noexcept
        : runtime_graph_{&runtime_graph}, evaluation_time_{evaluation_time} {}
    explicit OutputChangeContext(const EvaluationContext &evaluation_context) noexcept;

    RuntimeGraph &runtime_graph() const noexcept { return *runtime_graph_; }
    EvaluationTime evaluation_time() const noexcept;
    const RuntimeGraphInput *runtime_graph_inputs() const noexcept { return runtime_graph_inputs_; }
    OutputChangeContext for_graph(RuntimeGraph &graph) const noexcept {
        return OutputChangeContext{graph, evaluation_time_, nullptr};
    }

    void schedule_validity_check(RuntimeGraph &graph, std::size_t node_index) const noexcept;
    void schedule(RuntimeGraph &graph, std::size_t node_index) const noexcept;
    void notify_subscribers(details::DynamicSubscribers *subscribers, OutputChange change) const;

  private:
    OutputChangeContext(RuntimeGraph &runtime_graph, EvaluationTime evaluation_time,
                        const RuntimeGraphInput *runtime_graph_inputs) noexcept
        : runtime_graph_{&runtime_graph}, evaluation_time_{evaluation_time}, runtime_graph_inputs_{runtime_graph_inputs} {}

    RuntimeGraph *runtime_graph_;
    EvaluationTime evaluation_time_;
    const RuntimeGraphInput *runtime_graph_inputs_{};
};

using OutputChangeHandler = void (*)(OutputChangeContext &, const TimeSeriesRuntimeType *, const void *, OutputChange,
                                     std::span<const std::byte>);
using InputNotificationFunction = void (*)(OutputChangeContext &, std::size_t, std::size_t, OutputChange);
using RuntimeObserverFunction = void (*)(OutputChangeContext &, void *, OutputChange);

struct InputNotification {
    InputNotificationFunction function{};
    std::size_t data{};
};

class InputNotificationSubscriber {
  public:
    InputNotificationSubscriber(InputNotificationFunction function, std::size_t data, std::uint32_t node_index) noexcept
        : function_{function}, data_{data}, node_index_{node_index} {}

    void notify(OutputChangeContext &context, OutputChange change) const {
        function_(context, node_index_, data_, change);
    }

  private:
    InputNotificationFunction function_;
    std::size_t data_;
    std::uint32_t node_index_;
};

class ControlledInputSubscriber {
  public:
    ControlledInputSubscriber(std::uint32_t activity_offset, std::uint32_t node_index, bool required) noexcept
        : activity_offset_{activity_offset}, node_index_and_flags_{node_index | (required ? required_flag : 0)} {}

    std::size_t activity_offset() const noexcept { return activity_offset_; }
    std::size_t node_index() const noexcept { return node_index_and_flags_ & node_index_mask; }
    bool required() const noexcept { return (node_index_and_flags_ & required_flag) != 0; }

  private:
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t node_index_mask = required_flag - 1;

    std::uint32_t activity_offset_;
    std::uint32_t node_index_and_flags_;
};

void notify_input_subscribers(OutputChangeContext &context, const TimeSeriesRuntimeType *, const void *,
                              OutputChange change, std::span<const std::byte> records);
void notify_controlled_inputs(OutputChangeContext &context, const TimeSeriesRuntimeType *, const void *,
                              OutputChange change, std::span<const std::byte> records);

class OutputNotification {
  public:
    OutputNotification(OutputChangeHandler function, std::uint32_t record_bytes,
                       std::uint32_t following_bytes = 0) noexcept
        : function_{function}, record_bytes_{record_bytes}, following_bytes_{following_bytes} {}

    void notify(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type, const void *value,
                OutputChange change) const {
        // Adjacent blocks keep handler-specific records compact while allowing one output to serve several handlers.
        auto *handler = this;
        while (handler != nullptr) {
            handler->function_(context, runtime_type, value, change, handler->records());
            handler = handler->next();
        }
    }

    std::span<const std::byte> records() const noexcept {
        return {reinterpret_cast<const std::byte *>(this + 1), record_bytes_};
    }

    const std::byte *end() const noexcept { return records().data() + records().size() + following_bytes_; }

  private:
    const OutputNotification *next() const noexcept {
        if (following_bytes_ == 0) {
            return nullptr;
        }

        auto address = reinterpret_cast<std::uintptr_t>(records().data() + records().size());
        address = (address + alignof(OutputNotification) - 1) & ~(alignof(OutputNotification) - 1);
        return reinterpret_cast<const OutputNotification *>(address);
    }

    OutputChangeHandler function_;
    std::uint32_t record_bytes_;
    std::uint32_t following_bytes_;
};

static_assert(sizeof(OutputNotification) == 2 * sizeof(void *));
static_assert(sizeof(InputNotificationSubscriber) == 3 * sizeof(void *));
static_assert(sizeof(ControlledInputSubscriber) == sizeof(void *));

} // namespace bg
