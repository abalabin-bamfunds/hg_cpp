#pragma once

#include <bg/queue_handle.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/queue_mode.hpp>

#include <cstddef>
#include <memory>

namespace bg {

namespace util {
class buffer;
}

class TimeSeriesRuntimeType;
class TimeSeriesType;

namespace details {
class EngineWakeup;

void validate_queue_output_type(const TimeSeriesType &type, QueueMode mode);
} // namespace details

namespace details {

class QueueControl {
  public:
    // A non-null wakeup must outlive close() and all writes admitted by this controller.
    explicit QueueControl(EngineWakeup *wakeup = nullptr);
    QueueControl(const QueueControl &) = delete;
    QueueControl &operator=(const QueueControl &) = delete;
    QueueControl(QueueControl &&) = delete;
    QueueControl &operator=(QueueControl &&) = delete;
    ~QueueControl();

    // The buffer must be a concurrent chain with thread-safe allocation and deallocation; its allocator must outlive
    // retained handles. The runtime type must outlive close() and reader cleanup of the queue's messages.
    Queue make_queue(util::buffer buffer, std::size_t target, const TimeSeriesRuntimeType &runtime_type,
                     QueueMode mode = QueueMode::standard) const;

    // Rejects new writes, waits for accepted writes, and detaches the wakeup. A single controller calls close();
    // repeated sequential calls are safe.
    void close() noexcept;

  private:
    std::shared_ptr<QueueWriteGate> gate_;
};

} // namespace details

} // namespace bg

#include <bg/queue_node.hpp>
