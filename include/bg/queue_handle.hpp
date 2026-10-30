#pragma once

#include <bg/literal/time_series_literal.hpp>

#include <memory>

namespace bg {

namespace details {
class QueueMessageWriter;
class QueueWriteGate;
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

    // Returns false for a default, moved-from, or closed handle.
    bool is_open() const noexcept;

  private:
    void swap(Queue &other) noexcept;

    std::shared_ptr<details::QueueWriteGate> gate_;
    std::unique_ptr<details::QueueMessageWriter> writer_;
};

} // namespace bg
