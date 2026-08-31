#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>

namespace bg {

class RuntimeGraph;

enum class OutputChange : std::uint8_t {
    update,
    invalidation,
    revalidation,
};

class OutputNotificationContext {
  public:
    [[nodiscard]] RuntimeGraph &runtime_graph() const noexcept { return *runtime_graph_; }
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

class OutputNotification {
  public:
    OutputNotification(OutputNotificationFunction function, std::uint32_t record_bytes) noexcept
        : function_{function}, record_bytes_{record_bytes} {}

    void notify(OutputNotificationContext &context, const void *value, OutputChange change) const {
        function_(context, value, change, records());
    }

    [[nodiscard]] std::span<const std::byte> records() const noexcept {
        return {reinterpret_cast<const std::byte *>(this + 1), record_bytes_};
    }

    [[nodiscard]] const std::byte *end() const noexcept { return records().data() + records().size(); }

  private:
    OutputNotificationFunction function_;
    std::uint32_t record_bytes_;
};

static_assert(sizeof(OutputNotification) == 2 * sizeof(void *));

} // namespace bg
