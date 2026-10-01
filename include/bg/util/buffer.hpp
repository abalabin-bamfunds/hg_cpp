#pragma once

#include <bg/allocator.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::util {

enum class buffer_type { contiguous, single_writer_chain, concurrent_writer_chain };

namespace detail {

struct alignas(std::max_align_t) buffer_block {
    buffer_block(bg::allocator allocator, std::size_t capacity) : allocator{allocator}, capacity{capacity} {}

    std::byte *bytes() noexcept { return reinterpret_cast<std::byte *>(this) + sizeof(buffer_block); }
    const std::byte *bytes() const noexcept {
        return reinterpret_cast<const std::byte *>(this) + sizeof(buffer_block);
    }

    void retain() noexcept { references.fetch_add(1, std::memory_order_relaxed); }

    void release() noexcept {
        if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            auto memory = allocator;
            const auto size = sizeof(buffer_block) + capacity;
            std::destroy_at(this);
            memory.deallocate_bytes(this, size, alignof(buffer_block));
        }
    }

    bg::allocator allocator;
    const std::size_t capacity;
    std::atomic<std::size_t> committed{};
    std::atomic<std::size_t> sealed_end{};
    std::atomic<std::size_t> references{1};
    std::atomic<buffer_block *> next{};
    std::atomic<bool> writer_active{};
    std::size_t write_end{};
};

struct commit_slot {
    buffer_block *block{};
    std::uint32_t end{};
    std::atomic<bool> ready{};
};

inline constexpr std::uint64_t pack_mark(std::uint32_t position, std::uint32_t sequence) noexcept {
    return (static_cast<std::uint64_t>(sequence) << 32) | position;
}

inline constexpr std::uint32_t mark_position(std::uint64_t mark) noexcept {
    return static_cast<std::uint32_t>(mark);
}

inline constexpr std::uint32_t mark_sequence(std::uint64_t mark) noexcept {
    return static_cast<std::uint32_t>(mark >> 32);
}

inline std::shared_ptr<buffer_block> own_block(buffer_block *block, bg::allocator allocator) {
    block->retain();
    return std::shared_ptr<buffer_block>{block, [](buffer_block *value) noexcept { value->release(); },
                                         std::pmr::polymorphic_allocator<std::byte>{&allocator.memory_resource()}};
}

inline buffer_block *new_block(bg::allocator allocator, std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument{"buffer capacity must be positive"};
    }
    if (capacity > std::numeric_limits<std::size_t>::max() - sizeof(buffer_block)) {
        throw std::length_error{"buffer allocation size overflows"};
    }
    const auto allocation_size = sizeof(buffer_block) + capacity;
    auto *storage = allocator.allocate_bytes(allocation_size, alignof(buffer_block));
    return std::construct_at(static_cast<buffer_block *>(storage), allocator, capacity);
}

struct buffer_chain {
    buffer_chain(bg::allocator allocator, std::size_t block_capacity, buffer_type type, std::size_t ring_capacity)
        : allocator{allocator}, block_capacity{block_capacity}, type{type}, ring_capacity{ring_capacity},
          slots{type == buffer_type::concurrent_writer_chain ? ring_capacity : 0,
                &allocator.memory_resource()} {
        auto *first = new_block(allocator, block_capacity);
        head.store(first, std::memory_order_relaxed);
        try {
            tail.store(own_block(first, allocator), std::memory_order_relaxed);
        } catch (...) {
            head.store(nullptr, std::memory_order_relaxed);
            first->release();
            throw;
        }
    }

    ~buffer_chain() {
        auto *block = head.load(std::memory_order_relaxed);
        while (block != nullptr) {
            auto *next = block->next.load(std::memory_order_relaxed);
            block->release();
            block = next;
        }
    }

    buffer_block *append_block(std::size_t capacity) {
        auto *next = new_block(allocator, capacity);
        std::shared_ptr<buffer_block> tail_owner;
        try {
            tail_owner = own_block(next, allocator);
        } catch (...) {
            next->release();
            throw;
        }
        auto *previous = tail.load(std::memory_order_relaxed).get();
        previous->sealed_end.store(previous->write_end, std::memory_order_relaxed);
        previous->next.store(next, std::memory_order_release);
        tail.store(std::move(tail_owner), std::memory_order_release);
        return next;
    }

    void drain_commits() {
        std::lock_guard lock{drain_mutex};
        auto mark = committed_mark.load(std::memory_order_relaxed);
        while (true) {
            const auto sequence = mark_sequence(mark);
            auto &slot = slots[sequence & (ring_capacity - 1)];
            if (!slot.ready.load(std::memory_order_acquire)) {
                return;
            }

            const auto end = slot.end;
            slot.block->committed.store(end, std::memory_order_release);
            slot.block = nullptr;
            slot.end = 0;
            slot.ready.store(false, std::memory_order_relaxed);
            mark = pack_mark(static_cast<std::uint32_t>(end), sequence + 1);
            committed_mark.store(mark, std::memory_order_release);
            committed_mark.notify_all();
        }
    }

    bg::allocator allocator;
    const std::size_t block_capacity;
    const buffer_type type;
    const std::size_t ring_capacity;
    std::atomic<buffer_block *> head{};
    std::atomic<std::shared_ptr<buffer_block>> tail;
    std::pmr::vector<commit_slot> slots;
    std::mutex drain_mutex;
    std::mutex rollover_mutex;
    std::atomic<bool> rolling_over{};
    std::atomic<std::uint64_t> write_mark{};
    std::atomic<std::uint64_t> committed_mark{};
    std::atomic<bool> reader_active{};
    std::atomic<bool> writer_active{};
    std::size_t head_position{};
};

inline std::shared_ptr<buffer_chain> new_chain(bg::allocator allocator, std::size_t block_capacity,
                                               buffer_type type, std::size_t ring_capacity = 0) {
    if (block_capacity == 0) {
        throw std::invalid_argument{"buffer block capacity must be positive"};
    }
    if (type == buffer_type::concurrent_writer_chain && ring_capacity == 0) {
        throw std::invalid_argument{"commit ring capacity must be positive"};
    }
    if (type == buffer_type::concurrent_writer_chain && block_capacity > UINT32_MAX) {
        throw std::invalid_argument{"buffer chain blocks must fit a 32-bit position"};
    }
    if (type == buffer_type::concurrent_writer_chain && ring_capacity > UINT32_MAX) {
        throw std::invalid_argument{"commit ring capacity must fit a 32-bit sequence"};
    }
    if (type == buffer_type::concurrent_writer_chain && !std::has_single_bit(ring_capacity)) {
        throw std::invalid_argument{"commit ring capacity must be a power of two"};
    }
    return std::allocate_shared<buffer_chain>(std::pmr::polymorphic_allocator<buffer_chain>{&allocator.memory_resource()},
                                              allocator, block_capacity, type, ring_capacity);
}

} // namespace detail

class buffer {
  public:
    // The resource must outlive the buffer and all handles derived from it. Chains may allocate and
    // deallocate from writer and reader threads, so their resource must be thread-safe.
    static buffer contiguous(std::size_t capacity, bg::allocator allocator = {}) {
        buffer result{buffer_type::contiguous};
        result.block_ = detail::new_block(allocator, capacity);
        return result;
    }

    static buffer chain(std::size_t block_capacity, bg::allocator allocator = {}) {
        return buffer{detail::new_chain(allocator, block_capacity, buffer_type::single_writer_chain)};
    }

    static buffer concurrent_chain(std::size_t block_capacity, std::size_t commit_capacity,
                                   bg::allocator allocator = {}) {
        return buffer{detail::new_chain(allocator, block_capacity, buffer_type::concurrent_writer_chain,
                                        commit_capacity)};
    }

    buffer(const buffer &other)
        : type_{other.type_}, block_{other.block_}, chain_{other.chain_}, view_{other.view_}, view_size_{other.view_size_} {
        if (block_ != nullptr) {
            block_->retain();
        }
    }

    buffer(buffer &&other) noexcept
        : type_{other.type_}, block_{std::exchange(other.block_, nullptr)}, chain_{std::move(other.chain_)},
          view_{std::exchange(other.view_, nullptr)}, view_size_{std::exchange(other.view_size_, 0)} {}

    buffer &operator=(const buffer &other) {
        if (this == &other) {
            return *this;
        }
        buffer copy{other};
        swap(copy);
        return *this;
    }

    buffer &operator=(buffer &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        type_ = other.type_;
        block_ = std::exchange(other.block_, nullptr);
        chain_ = std::move(other.chain_);
        view_ = std::exchange(other.view_, nullptr);
        view_size_ = std::exchange(other.view_size_, 0);
        return *this;
    }

    ~buffer() { reset(); }

    buffer_type type() const noexcept { return type_; }

    std::size_t size() const {
        require_contiguous();
        return view_ == nullptr ? block_->committed.load(std::memory_order_acquire) : view_size_;
    }

    std::span<const std::byte> bytes() const {
        require_contiguous();
        auto *start = view_ == nullptr ? block_->bytes() : view_;
        return {start, size()};
    }

    buffer chunk(std::size_t begin, std::size_t end) const {
        require_contiguous();
        if (begin > end || end > size()) {
            throw std::out_of_range{"buffer chunk is outside committed bytes"};
        }
        buffer result{buffer_type::contiguous};
        result.block_ = block_;
        result.block_->retain();
        result.view_ = (view_ == nullptr ? block_->bytes() : view_) + begin;
        result.view_size_ = end - begin;
        return result;
    }

  private:
    explicit buffer(buffer_type type) : type_{type} {}
    explicit buffer(std::shared_ptr<detail::buffer_chain> chain)
        : type_{chain->type}, chain_{std::move(chain)} {}

    void require_contiguous() const {
        if (type_ != buffer_type::contiguous) {
            throw std::logic_error{"a buffer chain is not one contiguous range"};
        }
    }

    void reset() noexcept {
        if (block_ != nullptr) {
            block_->release();
            block_ = nullptr;
        }
        chain_.reset();
    }

    void swap(buffer &other) noexcept {
        std::swap(type_, other.type_);
        std::swap(block_, other.block_);
        chain_.swap(other.chain_);
        std::swap(view_, other.view_);
        std::swap(view_size_, other.view_size_);
    }

    buffer_type type_;
    detail::buffer_block *block_{};
    std::shared_ptr<detail::buffer_chain> chain_;
    std::byte *view_{};
    std::size_t view_size_{};

    friend class buffer_writer;
    friend class concurrent_buffer_writer;
    friend class buffer_reader;
};

class buffer_reservation {
  public:
    // Commit once before the writer is destroyed. Skipping a reservation hides later bytes.
    // Callers that construct objects in the bytes must destroy them before releasing the bytes.
    buffer_reservation(const buffer_reservation &) = delete;
    buffer_reservation &operator=(const buffer_reservation &) = delete;
    buffer_reservation(buffer_reservation &&other) noexcept
        : block_{std::exchange(other.block_, nullptr)}, begin_{other.begin_}, end_{other.end_},
          committed_{std::exchange(other.committed_, true)} {}
    buffer_reservation &operator=(buffer_reservation &&) = delete;

    std::span<std::byte> bytes() const noexcept { return {block_->bytes() + begin_, end_ - begin_}; }

    void commit() {
        if (committed_ || block_ == nullptr) {
            throw std::logic_error{"buffer reservation already committed"};
        }
        if (block_->committed.load(std::memory_order_relaxed) != begin_) {
            throw std::logic_error{"single-writer reservations must commit in reservation order"};
        }
        block_->committed.store(end_, std::memory_order_release);
        committed_ = true;
    }

  private:
    buffer_reservation(detail::buffer_block *block, std::size_t begin, std::size_t end)
        : block_{block}, begin_{begin}, end_{end} {}

    detail::buffer_block *block_;
    std::size_t begin_;
    std::size_t end_;
    bool committed_{};

    friend class buffer_writer;
};

class buffer_writer {
  public:
    explicit buffer_writer(buffer &buffer) : buffer_{buffer} {
        if (buffer_.type_ == buffer_type::contiguous) {
            if (buffer_.view_ != nullptr) {
                throw std::invalid_argument{"a chunk is not writable"};
            }
            block_ = buffer_.block_;
            acquire(block_->writer_active);
            position_ = block_->write_end;
        } else if (buffer_.type_ == buffer_type::single_writer_chain) {
            acquire(buffer_.chain_->writer_active);
            block_ = buffer_.chain_->tail.load(std::memory_order_acquire).get();
            position_ = block_->write_end;
        } else {
            throw std::invalid_argument{"use concurrent_buffer_writer for a concurrent buffer chain"};
        }
    }

    buffer_writer(const buffer_writer &) = delete;
    buffer_writer &operator=(const buffer_writer &) = delete;
    buffer_writer(buffer_writer &&) = delete;
    buffer_writer &operator=(buffer_writer &&) = delete;

    ~buffer_writer() {
        if (buffer_.type_ == buffer_type::contiguous && block_ != nullptr) {
            block_->writer_active.store(false, std::memory_order_release);
        } else if (buffer_.chain_) {
            buffer_.chain_->writer_active.store(false, std::memory_order_release);
        }
    }

    buffer_reservation reserve(std::size_t size) {
        if (size == 0) {
            throw std::invalid_argument{"buffer reservation must be positive"};
        }
        if (buffer_.type_ == buffer_type::contiguous && size > block_->capacity - position_) {
            throw std::length_error{"contiguous buffer has insufficient capacity"};
        }
        if (buffer_.type_ == buffer_type::single_writer_chain && size > block_->capacity - position_) {
            block_ = buffer_.chain_->append_block(std::max(buffer_.chain_->block_capacity, size));
            position_ = 0;
        }

        const auto begin = position_;
        position_ += size;
        block_->write_end = position_;
        return {block_, begin, position_};
    }

  private:
    static void acquire(std::atomic<bool> &active) {
        bool expected = false;
        if (!active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            throw std::logic_error{"a buffer supports only one active writer"};
        }
    }

    buffer buffer_;
    detail::buffer_block *block_{};
    std::size_t position_{};
};

class concurrent_buffer_reservation {
  public:
    // Commit every reservation exactly once so the ordered commit mark can advance. Callers
    // that construct objects in the bytes must destroy them before releasing the bytes.
    concurrent_buffer_reservation(const concurrent_buffer_reservation &) = delete;
    concurrent_buffer_reservation &operator=(const concurrent_buffer_reservation &) = delete;
    concurrent_buffer_reservation(concurrent_buffer_reservation &&other) noexcept
        : chain_{std::move(other.chain_)}, sequence_{other.sequence_},
          block_{std::exchange(other.block_, nullptr)}, begin_{other.begin_}, end_{other.end_},
          committed_{std::exchange(other.committed_, true)} {}
    concurrent_buffer_reservation &operator=(concurrent_buffer_reservation &&) = delete;

    std::span<std::byte> bytes() const noexcept { return {block_->bytes() + begin_, end_ - begin_}; }

    void commit() {
        if (committed_ || block_ == nullptr) {
            throw std::logic_error{"buffer reservation already committed"};
        }

        auto &slot = chain_->slots[sequence_ & (chain_->ring_capacity - 1)];
        slot.ready.store(true, std::memory_order_release);
        chain_->drain_commits();
        committed_ = true;
    }

  private:
    concurrent_buffer_reservation(std::shared_ptr<detail::buffer_chain> chain, std::uint64_t sequence,
                                  detail::buffer_block *block, std::size_t begin, std::size_t end)
        : chain_{std::move(chain)}, sequence_{sequence}, block_{block}, begin_{begin}, end_{end} {}

    std::shared_ptr<detail::buffer_chain> chain_;
    std::uint64_t sequence_;
    detail::buffer_block *block_;
    std::size_t begin_;
    std::size_t end_;
    bool committed_{};

    friend class concurrent_buffer_writer;
};

class concurrent_buffer_writer {
  public:
    explicit concurrent_buffer_writer(buffer &buffer) : chain_{buffer.chain_} {
        if (buffer.type_ != buffer_type::concurrent_writer_chain) {
            throw std::invalid_argument{"concurrent_buffer_writer requires a concurrent buffer chain"};
        }
    }

    concurrent_buffer_reservation reserve(std::size_t size) {
        if (size == 0) {
            throw std::invalid_argument{"buffer reservation must be positive"};
        }
        if (size > UINT32_MAX) {
            throw std::length_error{"a concurrent reservation must fit a 32-bit block position"};
        }

        while (true) {
            if (chain_->rolling_over.load(std::memory_order_acquire)) {
                chain_->rolling_over.wait(true, std::memory_order_acquire);
                continue;
            }

            auto mark = chain_->write_mark.load(std::memory_order_acquire);
            const auto sequence = detail::mark_sequence(mark);
            const auto committed = chain_->committed_mark.load(std::memory_order_acquire);
            if (sequence - detail::mark_sequence(committed) >= chain_->ring_capacity) {
                chain_->committed_mark.wait(committed, std::memory_order_acquire);
                continue;
            }

            auto block_owner = chain_->tail.load(std::memory_order_acquire);
            auto *block = block_owner.get();
            if (chain_->rolling_over.load(std::memory_order_acquire) ||
                block != chain_->tail.load(std::memory_order_acquire).get()) {
                continue;
            }

            const auto begin = detail::mark_position(mark);
            if (size <= block->capacity - begin) {
                const auto end = begin + static_cast<std::uint32_t>(size);
                const auto next = detail::pack_mark(end, sequence + 1);
                if (!chain_->write_mark.compare_exchange_weak(mark, next, std::memory_order_acq_rel,
                                                              std::memory_order_acquire)) {
                    continue;
                }

                auto &slot = chain_->slots[sequence & (chain_->ring_capacity - 1)];
                slot.block = block;
                slot.end = end;
                return {chain_, sequence, block, begin, end};
            }

            auto reservation = reserve_after_rollover(size);
            if (reservation) {
                return std::move(*reservation);
            }
        }
    }

  private:
    std::optional<concurrent_buffer_reservation> reserve_after_rollover(std::size_t size) {
        std::unique_lock lock{chain_->rollover_mutex};
        chain_->rolling_over.store(true, std::memory_order_release);
        auto release_rollover = bg::util::scope_exit{[&]() noexcept {
            chain_->rolling_over.store(false, std::memory_order_release);
            chain_->rolling_over.notify_all();
        }};

        while (true) {
            const auto mark = chain_->write_mark.load(std::memory_order_acquire);
            const auto sequence = detail::mark_sequence(mark);
            const auto committed = chain_->committed_mark.load(std::memory_order_acquire);
            if (sequence - detail::mark_sequence(committed) >= chain_->ring_capacity) {
                chain_->rolling_over.store(false, std::memory_order_release);
                chain_->rolling_over.notify_all();
                lock.unlock();
                chain_->committed_mark.wait(committed, std::memory_order_acquire);
                return std::nullopt;
            }

            auto block_owner = chain_->tail.load(std::memory_order_acquire);
            auto *block = block_owner.get();
            const auto begin = detail::mark_position(mark);
            if (size <= block->capacity - begin) {
                chain_->rolling_over.store(false, std::memory_order_release);
                chain_->rolling_over.notify_all();
                return std::nullopt;
            }

            const auto capacity = std::max(chain_->block_capacity, size);
            auto *next_block = detail::new_block(chain_->allocator, capacity);
            std::shared_ptr<detail::buffer_block> next_owner;
            try {
                next_owner = detail::own_block(next_block, chain_->allocator);
            } catch (...) {
                next_block->release();
                throw;
            }
            const auto next = detail::pack_mark(static_cast<std::uint32_t>(size), sequence + 1);
            auto expected = mark;
            if (!chain_->write_mark.compare_exchange_strong(expected, next, std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
                next_owner.reset();
                next_block->release();
                continue;
            }

            block->sealed_end.store(begin, std::memory_order_relaxed);
            block->next.store(next_block, std::memory_order_release);
            chain_->tail.store(std::move(next_owner), std::memory_order_release);

            auto &slot = chain_->slots[sequence & (chain_->ring_capacity - 1)];
            slot.block = next_block;
            slot.end = static_cast<std::uint32_t>(size);
            chain_->rolling_over.store(false, std::memory_order_release);
            chain_->rolling_over.notify_all();
            return concurrent_buffer_reservation{chain_, sequence, next_block, 0, size};
        }
    }

    std::shared_ptr<detail::buffer_chain> chain_;
};

class buffer_reader {
  public:
    explicit buffer_reader(buffer &buffer) : buffer_{buffer} {
        if (buffer_.type_ == buffer_type::contiguous) {
            block_ = buffer_.block_;
            position_ = buffer_.view_ == nullptr ? 0 : static_cast<std::size_t>(buffer_.view_ - block_->bytes());
            limit_ = buffer_.view_ == nullptr ? block_->capacity : position_ + buffer_.view_size_;
        } else {
            bool expected = false;
            if (!buffer_.chain_->reader_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                throw std::logic_error{"a buffer chain supports only one active reader"};
            }
            block_ = buffer_.chain_->head.load(std::memory_order_acquire);
            position_ = buffer_.chain_->head_position;
        }
    }

    buffer_reader(const buffer_reader &) = delete;
    buffer_reader &operator=(const buffer_reader &) = delete;
    buffer_reader(buffer_reader &&) = delete;
    buffer_reader &operator=(buffer_reader &&) = delete;

    ~buffer_reader() {
        if (buffer_.type_ != buffer_type::contiguous) {
            buffer_.chain_->head_position = position_;
            buffer_.chain_->reader_active.store(false, std::memory_order_release);
        }
    }

    std::span<const std::byte> readable() {
        if (buffer_.type_ == buffer_type::contiguous) {
            const auto end = std::min(limit_, block_->committed.load(std::memory_order_acquire));
            return {block_->bytes() + position_, end > position_ ? end - position_ : 0};
        }

        while (block_ != nullptr) {
            const auto end = block_->committed.load(std::memory_order_acquire);
            if (position_ < end) {
                return {block_->bytes() + position_, end - position_};
            }

            auto *next = block_->next.load(std::memory_order_acquire);
            if (next == nullptr || end < block_->sealed_end.load(std::memory_order_acquire)) {
                return {};
            }

            buffer_.chain_->head.store(next, std::memory_order_release);
            buffer_.chain_->head_position = 0;
            block_->release();
            block_ = next;
            position_ = 0;
        }
        return {};
    }

    buffer current_buffer() {
        const auto available = readable();
        if (available.empty()) {
            throw std::logic_error{"the reader has no committed bytes"};
        }

        buffer result{buffer_type::contiguous};
        result.block_ = block_;
        result.block_->retain();
        result.view_ = block_->bytes() + position_;
        result.view_size_ = available.size();
        return result;
    }

    void consume(std::size_t size) {
        const auto available = readable().size();
        if (size > available) {
            throw std::out_of_range{"cannot consume bytes that are not readable"};
        }
        position_ += size;
    }

  private:
    buffer buffer_;
    detail::buffer_block *block_{};
    std::size_t position_{};
    std::size_t limit_{};
};

using concurrent_buffer_reader = buffer_reader;

} // namespace bg::util
