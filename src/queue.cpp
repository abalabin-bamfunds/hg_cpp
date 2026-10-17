#include <bg/queue.hpp>

#include <bg/allocator.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/buffer.hpp>
#include <bg/util/scope_exit.hpp>

#include <atomic>
#include <limits>
#include <memory_resource>
#include <utility>

namespace bg::details {

class QueueWriteGate {
  public:
    bool enter() noexcept {
        auto current = state_.load(std::memory_order_acquire);
        while ((current & closed_bit) == 0) {
            if (state_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void leave() noexcept {
        const auto previous = state_.fetch_sub(1, std::memory_order_release);
        if ((previous & closed_bit) != 0 && (previous & active_mask) == 1) {
            state_.notify_all();
        }
    }

    bool closed() const noexcept { return (state_.load(std::memory_order_acquire) & closed_bit) != 0; }

    void close() noexcept {
        state_.fetch_or(closed_bit, std::memory_order_acq_rel);
        auto current = state_.load(std::memory_order_acquire);
        while ((current & active_mask) != 0) {
            state_.wait(current, std::memory_order_acquire);
            current = state_.load(std::memory_order_acquire);
        }
    }

  private:
    static constexpr std::size_t closed_bit = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    static constexpr std::size_t active_mask = ~closed_bit;

    std::atomic<std::size_t> state_{};
};

} // namespace bg::details

namespace bg {

Queue::Queue(std::shared_ptr<details::QueueWriteGate> gate,
             std::unique_ptr<details::QueueMessageWriter> writer) noexcept
    : gate_{std::move(gate)}, writer_{std::move(writer)} {}

Queue::Queue() noexcept = default;

Queue::Queue(const Queue &other) : gate_{other.gate_} {
    if (gate_ && other.writer_ && !gate_->closed()) {
        writer_ = std::make_unique<details::QueueMessageWriter>(*other.writer_);
    }
}

Queue &Queue::operator=(const Queue &other) {
    if (this != &other) {
        auto copy = Queue{other};
        swap(copy);
    }
    return *this;
}

Queue::Queue(Queue &&other) noexcept = default;
Queue &Queue::operator=(Queue &&other) noexcept = default;
Queue::~Queue() = default;

bool Queue::write(const literal::TimeSeriesLiteral &literal) {
    if (!gate_ || !writer_ || !gate_->enter()) {
        return false;
    }

    auto leave = util::scope_exit{[&]() noexcept { gate_->leave(); }};
    return writer_->write(literal);
}

void Queue::swap(Queue &other) noexcept {
    gate_.swap(other.gate_);
    writer_.swap(other.writer_);
}

namespace details {

QueueControl::QueueControl() : gate_{std::make_shared<QueueWriteGate>()} {}

QueueControl::~QueueControl() { close(); }

Queue QueueControl::make_queue(util::buffer buffer, std::size_t target,
                               const TimeSeriesRuntimeType &runtime_type, QueueMode mode) const {
    if (gate_->closed()) {
        return Queue{gate_, {}};
    }

    auto writer = std::make_unique<QueueMessageWriter>(std::move(buffer), target, runtime_type,
                                                       allocator{*std::pmr::new_delete_resource()}, mode);
    return Queue{gate_, std::move(writer)};
}

void QueueControl::close() noexcept {
    gate_->close();
}

} // namespace details

} // namespace bg
