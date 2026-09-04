#pragma once

#include <bg/evaluation_time.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>

namespace bg {

class RuntimeGraph;
class Node;

enum class OutputChange : std::uint8_t {
    update,
    invalidation,
    revalidation,
};

class OutputNotificationContext {
  public:
    [[nodiscard]] RuntimeGraph &runtime_graph() const noexcept { return *runtime_graph_; }
    [[nodiscard]] EvaluationTime evaluation_time() const noexcept;
    [[nodiscard]] Node node(std::size_t node_index) const noexcept;
    [[nodiscard]] void *runtime_storage_at(std::size_t offset) const noexcept;
    [[nodiscard]] std::pmr::memory_resource &memory_resource(RuntimeGraph &graph) const noexcept;

    void schedule_validity_check(RuntimeGraph &graph, std::size_t node_index) const noexcept;

  private:
    friend class RuntimeGraph;

    explicit OutputNotificationContext(RuntimeGraph &runtime_graph) noexcept : runtime_graph_{&runtime_graph} {}

    RuntimeGraph *runtime_graph_;
};

using OutputNotificationFunction = void (*)(OutputNotificationContext &, const void *, OutputChange,
                                            std::span<const std::byte>);
using InputNotificationFunction = void (*)(OutputNotificationContext &, std::size_t, std::size_t, OutputChange);

struct InputNotificationCallback {
    InputNotificationFunction function{};
    std::size_t data{};
};

class CallbackInputSubscriber {
  public:
    CallbackInputSubscriber(InputNotificationFunction function, std::size_t data, std::uint32_t node_index) noexcept
        : function_{function}, data_{data}, node_index_{node_index} {}

    void notify(OutputNotificationContext &context, OutputChange change) const {
        function_(context, node_index_, data_, change);
    }

  private:
    InputNotificationFunction function_;
    std::size_t data_;
    std::uint32_t node_index_;
};

void notify_callback_inputs(OutputNotificationContext &context, const void *, OutputChange change,
                            std::span<const std::byte> records);

class OutputNotification {
  public:
    OutputNotification(OutputNotificationFunction function, std::uint32_t record_bytes,
                       std::uint32_t following_bytes = 0) noexcept
        : function_{function}, record_bytes_{record_bytes}, following_bytes_{following_bytes} {}

    void notify(OutputNotificationContext &context, const void *value, OutputChange change) const {
        // Adjacent blocks keep handler-specific records compact while allowing one output to serve several handlers.
        auto *notification = this;
        while (notification != nullptr) {
            notification->function_(context, value, change, notification->records());
            notification = notification->next();
        }
    }

    [[nodiscard]] std::span<const std::byte> records() const noexcept {
        return {reinterpret_cast<const std::byte *>(this + 1), record_bytes_};
    }

    [[nodiscard]] const std::byte *end() const noexcept {
        return records().data() + records().size() + following_bytes_;
    }

  private:
    [[nodiscard]] const OutputNotification *next() const noexcept {
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
static_assert(sizeof(CallbackInputSubscriber) == 3 * sizeof(void *));

} // namespace bg
