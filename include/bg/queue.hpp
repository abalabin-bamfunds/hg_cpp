#pragma once

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
class QueueMessageWriter;
class QueueWriteGate;

void validate_queue_output_type(const TimeSeriesType &type, QueueMode mode);
} // namespace details

// A queue handle writes to one destination. Copying creates an independent writer cursor. Distinct handles may write
// concurrently. Do not copy a handle during a write through it.
class Queue {
  public:
    // Engine construction API. The gate owns write admission and the writer owns a cursor into the supplied chain.
    Queue(std::shared_ptr<details::QueueWriteGate> gate, std::unique_ptr<details::QueueMessageWriter> writer) noexcept;

    Queue() noexcept;
    Queue(const Queue &other);
    Queue &operator=(const Queue &other);
    Queue(Queue &&other) noexcept;
    Queue &operator=(Queue &&other) noexcept;
    ~Queue();

    // Returns false for a default, moved-from, or closed handle and for root SKIP. Serialization errors propagate
    // while open; closure rejects a write before it inspects the literal.
    bool write(const ::literal::TimeSeriesLiteral &literal);

  private:
    void swap(Queue &other) noexcept;

    std::shared_ptr<details::QueueWriteGate> gate_;
    std::unique_ptr<details::QueueMessageWriter> writer_;
};

namespace details {

class QueueControl {
  public:
    QueueControl();
    QueueControl(const QueueControl &) = delete;
    QueueControl &operator=(const QueueControl &) = delete;
    QueueControl(QueueControl &&) = delete;
    QueueControl &operator=(QueueControl &&) = delete;
    ~QueueControl();

    // The buffer must be a concurrent chain with thread-safe allocation and deallocation; its allocator must outlive
    // retained handles. The runtime type must outlive close() and reader cleanup of the queue's messages.
    Queue make_queue(util::buffer buffer, std::size_t target, const TimeSeriesRuntimeType &runtime_type,
                     QueueMode mode = QueueMode::standard) const;

    // Rejects new writes and waits for accepted writes to finish. Safe to call more than once.
    void close() noexcept;

  private:
    std::shared_ptr<QueueWriteGate> gate_;
};

} // namespace details

} // namespace bg

#include <bg/queue_node.hpp>
