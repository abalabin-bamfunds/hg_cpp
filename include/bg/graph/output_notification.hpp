#pragma once

#include <bg/evaluation_time.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bg {

class RuntimeGraph;
class TimeSeriesRuntimeType;
namespace details {
struct DynamicSubscribers;
}

enum class OutputChange : std::uint8_t {
    update,
    invalidation,
    revalidation,
};

class OutputChangeContext {
  public:
    RuntimeGraph &runtime_graph() const noexcept { return *runtime_graph_; }
    EvaluationTime evaluation_time() const noexcept;
    OutputChangeContext for_graph(RuntimeGraph &graph) const noexcept {
        return OutputChangeContext{graph, evaluation_time_};
    }

    void schedule_validity_check(RuntimeGraph &graph, std::size_t node_index) const noexcept;
    void notify(details::DynamicSubscribers *subscribers, OutputChange change) const;

  private:
    friend class RuntimeGraph;

    explicit OutputChangeContext(RuntimeGraph &runtime_graph) noexcept;
    OutputChangeContext(RuntimeGraph &runtime_graph, EvaluationTime evaluation_time) noexcept
        : runtime_graph_{&runtime_graph}, evaluation_time_{evaluation_time} {}

    RuntimeGraph *runtime_graph_;
    EvaluationTime evaluation_time_;
};

using OutputNotificationFunction = void (*)(OutputChangeContext &, const TimeSeriesRuntimeType *, const void *,
                                            OutputChange, std::span<const std::byte>);
using InputChangeFunction = void (*)(OutputChangeContext &, std::size_t, std::size_t, OutputChange);
using DynamicObserverFunction = void (*)(OutputChangeContext &, void *, OutputChange);

struct InputChangeCallback {
    InputChangeFunction function{};
    std::size_t data{};
};

class InputCallbackSubscriber {
  public:
    InputCallbackSubscriber(InputChangeFunction function, std::size_t data, std::uint32_t node_index) noexcept
        : function_{function}, data_{data}, node_index_{node_index} {}

    void notify(OutputChangeContext &context, OutputChange change) const {
        function_(context, node_index_, data_, change);
    }

  private:
    InputChangeFunction function_;
    std::size_t data_;
    std::uint32_t node_index_;
};

void notify_input_callbacks(OutputChangeContext &context, const TimeSeriesRuntimeType *, const void *,
                            OutputChange change, std::span<const std::byte> records);

class OutputNotification {
  public:
    OutputNotification(OutputNotificationFunction function, std::uint32_t record_bytes,
                       std::uint32_t following_bytes = 0) noexcept
        : function_{function}, record_bytes_{record_bytes}, following_bytes_{following_bytes} {}

    void notify(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type, const void *value,
                OutputChange change) const {
        // Adjacent blocks keep handler-specific records compact while allowing one output to serve several handlers.
        auto *notification = this;
        while (notification != nullptr) {
            notification->function_(context, runtime_type, value, change, notification->records());
            notification = notification->next();
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

    OutputNotificationFunction function_;
    std::uint32_t record_bytes_;
    std::uint32_t following_bytes_;
};

static_assert(sizeof(OutputNotification) == 2 * sizeof(void *));
static_assert(sizeof(InputCallbackSubscriber) == 3 * sizeof(void *));

} // namespace bg
