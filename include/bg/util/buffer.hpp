#pragma once

#include <bg/allocator.hpp>
#include <bg/util/details/buffer_blocks.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg::util {

enum class buffer_type { contiguous, single_writer_chain, concurrent_writer_chain };

class buffer_reader;
class buffer_writer;

class buffer {
  public:
    // The allocator must outlive all handles. Concurrent chains require thread-safe allocation and deallocation.
    // A retained chain handle pins its block and successors; move the root into a cursor to release prefixes.
    static buffer contiguous(std::size_t capacity, bg::allocator allocator = {}) {
        return buffer{details::buffer_block::create(allocator, capacity)};
    }

    static buffer chain(std::size_t block_capacity, bg::allocator allocator = {}) {
        return buffer{details::single_writer_block::create(allocator, block_capacity, block_capacity)};
    }

    static buffer concurrent_chain(std::size_t block_capacity, std::size_t commit_capacity,
                                   bg::allocator allocator = {}) {
        return buffer{details::new_concurrent_writer_block(allocator, block_capacity, commit_capacity)};
    }

    buffer(const buffer &other) : control_{other.control_}, begin_{other.begin_}, end_{other.end_} { retain(); }
    buffer(buffer &&other) noexcept
        : control_{std::exchange(other.control_, nullptr)}, begin_{std::exchange(other.begin_, 0)},
          end_{std::exchange(other.end_, 0)} {}
    buffer &operator=(const buffer &other) {
        if (this != &other) {
            auto copy = other;
            swap(copy);
        }
        return *this;
    }
    buffer &operator=(buffer &&other) noexcept {
        if (this != &other) {
            reset();
            control_ = std::exchange(other.control_, nullptr);
            begin_ = std::exchange(other.begin_, 0);
            end_ = std::exchange(other.end_, 0);
        }
        return *this;
    }
    ~buffer() { reset(); }

    buffer_type type() const noexcept {
        if (end_ != 0 || control_.holds<details::buffer_block>()) {
            return buffer_type::contiguous;
        }
        return control_.holds<details::single_writer_block>() ? buffer_type::single_writer_chain
                                                             : buffer_type::concurrent_writer_chain;
    }

    std::size_t size() const {
        require_contiguous();
        auto *block = base_block();
        const auto begin = data_begin();
        const auto end =
            std::min(end_ == 0 ? block_end(block) : end_, block->committed.load(std::memory_order_acquire));
        return end > begin ? end - begin : 0;
    }

    std::span<const std::byte> bytes() const {
        require_contiguous();
        return {base_block()->at(data_begin()), size()};
    }

    buffer chunk(std::size_t begin, std::size_t end) const {
        require_contiguous();
        if (begin > end || end > size()) {
            throw std::out_of_range{"buffer chunk is outside committed bytes"};
        }
        const auto offset = data_begin() + begin;
        return buffer{block_pointer(), offset, offset + end - begin};
    }

  private:
    buffer() = default;
    template <typename Block> explicit buffer(Block *block) : control_{block} {}
    buffer(details::block_pointer block, std::size_t begin, std::size_t end) : begin_{begin}, end_{end} {
        block.visit([&](auto *value) { control_ = value; });
        retain();
    }

    void require_contiguous() const {
        if (type() != buffer_type::contiguous) {
            throw std::logic_error{"a buffer chain is not one contiguous range"};
        }
    }

    details::buffer_block *base_block() const noexcept { return block_pointer().base_block(); }
    details::block_pointer block_pointer() const noexcept {
        return control_.visit([](auto *block) -> details::block_pointer { return block; });
    }
    details::block_pointer detach_block() noexcept {
        const auto block = block_pointer();
        control_.clear();
        return block;
    }
    std::size_t data_begin() const noexcept { return begin_ == 0 ? block_pointer().data_offset() : begin_; }
    static std::size_t block_end(const details::buffer_block *block) noexcept {
        return sizeof(details::buffer_block) + block->capacity;
    }
    void retain() noexcept {
        if (control_) {
            control_.visit([](auto *block) { block->retain(); });
        }
    }
    void reset() noexcept {
        if (control_) {
            auto block = block_pointer();
            control_.clear();
            details::block_reference::adopt(block).reset();
        }
    }
    void swap(buffer &other) noexcept {
        std::swap(control_, other.control_);
        std::swap(begin_, other.begin_);
        std::swap(end_, other.end_);
    }

    details::buffer_control control_;
    std::size_t begin_{};
    std::size_t end_{};

    friend class buffer_writer;
    friend class concurrent_buffer_writer;
    friend class buffer_reader;
};

static_assert(sizeof(buffer) == sizeof(void *) + 2 * sizeof(std::size_t));

// Typed writes construct objects in stream order. Readers must follow that type sequence and read or skip each
// nontrivial object exactly once. Destroy uncommitted objects before abandoning a write; the buffer does not track
// object lifetimes.
class buffer_reservation {
  public:
    // A reservation must not outlive its writer. Do not overlap reservations from the same writer.
    buffer_reservation(const buffer_reservation &) = delete;
    buffer_reservation &operator=(const buffer_reservation &) = delete;
    buffer_reservation(buffer_reservation &&other) noexcept
        : block_{std::exchange(other.block_, nullptr)}, begin_{other.begin_}, cursor_{other.cursor_}, end_{other.end_} {}
    buffer_reservation &operator=(buffer_reservation &&) = delete;

    std::span<std::byte> bytes() const noexcept {
        return block_ == nullptr ? std::span<std::byte>{} : std::span<std::byte>{block_->at(begin_), end_ - begin_};
    }

    void write_bytes(std::span<const std::byte> bytes) {
        if (bytes.empty()) {
            return;
        }
        require_space(bytes.size());
        std::memcpy(block_->at(cursor_), bytes.data(), bytes.size());
        cursor_ += bytes.size();
    }

    template <typename T, typename... Args> T &emplace(Args &&...args) {
        details::require_supported_alignment(alignof(T));
        const auto begin = cursor_ + details::alignment_padding(cursor_, alignof(T));
        require_space(begin - cursor_ + sizeof(T));
        auto *value = std::construct_at(reinterpret_cast<T *>(block_->at(begin)), std::forward<Args>(args)...);
        cursor_ = begin + sizeof(T);
        return *value;
    }

    template <typename T> auto &write(T &&value) {
        return emplace<std::remove_cvref_t<T>>(std::forward<T>(value));
    }

    void commit() {
        if (block_ == nullptr) {
            throw std::logic_error{"buffer reservation already committed"};
        }
        block_->committed.store(end_, std::memory_order_release);
        block_ = nullptr;
    }

  private:
    buffer_reservation(details::buffer_block *block, std::size_t begin, std::size_t end)
        : block_{block}, begin_{begin}, cursor_{begin}, end_{end} {}

    void require_space(std::size_t size) const {
        if (block_ == nullptr || size > end_ - cursor_) {
            throw std::length_error{"buffer reservation has insufficient space"};
        }
    }

    details::buffer_block *block_{};
    std::size_t begin_{};
    std::size_t cursor_{};
    std::size_t end_{};
    friend class buffer_writer;
};

class buffer_writer {
  public:
    // Use one writer per buffer. Commit an unsized write before making a sized reservation, and vice versa.
    explicit buffer_writer(buffer buffer) {
        if (buffer.type() == buffer_type::contiguous) {
            auto *block = buffer.control_.get<details::buffer_block>();
            if (block == nullptr || buffer.begin_ != 0 || buffer.end_ != 0) {
                throw std::invalid_argument{"a chunk is not writable"};
            }
            block_ = details::block_reference::adopt(buffer.detach_block());
            position_ = block->committed.load(std::memory_order_acquire);
            return;
        }
        if (buffer.control_.get<details::single_writer_block>() == nullptr || buffer.end_ != 0) {
            throw std::invalid_argument{"use concurrent_buffer_writer for a concurrent buffer chain"};
        }
        block_ = details::block_reference::adopt(buffer.detach_block());
        auto *single = block_.get<details::single_writer_block>();
        while (auto *next = single->next_block()) {
            block_ = details::block_reference{next};
            single = next;
        }
        position_ = single->committed.load(std::memory_order_acquire);
    }

    buffer_writer(const buffer_writer &) = delete;
    buffer_writer &operator=(const buffer_writer &) = delete;
    buffer_writer(buffer_writer &&) = delete;
    buffer_writer &operator=(buffer_writer &&) = delete;

    // Discard the current uncommitted write after destroying any typed values it contains.
    void rollback() noexcept {
        if (pending_tail_ != nullptr) {
            pending_first_.reset();
            pending_tail_ = nullptr;
            starting_block_end_ = 0;
        }
        position_ = block_.pointer().base_block()->committed.load(std::memory_order_relaxed);
    }

    // Cleanup can inspect the uncommitted prefix only during this synchronous call.
    template <typename Cleanup> void rollback(Cleanup &&cleanup);

    // Writes remain private until commit. Raw bytes may span blocks; a typed object always occupies one block.
    void write_bytes(std::span<const std::byte> bytes) {
        if (bytes.empty()) {
            return;
        }

        if (auto *contiguous = block_.get<details::buffer_block>()) {
            if (bytes.size() > buffer::block_end(contiguous) - position_) {
                throw std::length_error{"contiguous buffer has insufficient capacity"};
            }
        }

        while (!bytes.empty()) {
            auto *block = pending_tail_ == nullptr ? block_.pointer().base_block() : pending_tail_;
            const auto available = buffer::block_end(block) - position_;
            if (available == 0) {
                advance_to_next_block(1);
                continue;
            }

            const auto count = std::min(available, bytes.size());
            std::memcpy(block->at(position_), bytes.data(), count);
            position_ += count;
            bytes = bytes.subspan(count);
        }
    }

    template <typename T, typename... Args> T &emplace(Args &&...args) {
        details::require_supported_alignment(alignof(T));

        auto *block = pending_tail_ == nullptr ? block_.pointer().base_block() : pending_tail_;
        auto begin = position_ + details::alignment_padding(position_, alignof(T));
        const auto end = buffer::block_end(block);
        if (begin > end || sizeof(T) > end - begin) {
            const auto next_begin = sizeof(details::single_writer_block);
            const auto required = details::alignment_padding(next_begin, alignof(T)) + sizeof(T);
            advance_to_next_block(required);
            block = pending_tail_;
            begin = position_ + details::alignment_padding(position_, alignof(T));
        }

        auto *value = std::construct_at(reinterpret_cast<T *>(block->at(begin)), std::forward<Args>(args)...);
        position_ = begin + sizeof(T);
        return *value;
    }

    template <typename T> auto &write(T &&value) {
        return emplace<std::remove_cvref_t<T>>(std::forward<T>(value));
    }

    void commit() noexcept {
        if (pending_tail_ == nullptr) {
            block_.pointer().base_block()->committed.store(position_, std::memory_order_release);
            return;
        }

        auto *start = block_.get<details::single_writer_block>();
        auto *last = pending_tail_;
        auto last_owner = details::block_reference{last};
        last->committed.store(position_, std::memory_order_release);

        start->sealed_end = starting_block_end_;
        const auto previous_end = start->committed.load(std::memory_order_relaxed);
        // The link publishes a write that began in a full block; otherwise the new committed end publishes it.
        start->next.store(pending_first_.get<details::single_writer_block>(), std::memory_order_release);
        pending_first_.relinquish();
        if (starting_block_end_ > previous_end) {
            start->committed.store(starting_block_end_, std::memory_order_release);
        }

        block_ = std::move(last_owner);
        pending_tail_ = nullptr;
    }

    buffer_reservation reserve(std::size_t size, std::size_t alignment = 1) {
        if (size == 0) {
            throw std::invalid_argument{"buffer reservation must be positive"};
        }
        details::require_supported_alignment(alignment);
        if (pending_tail_ != nullptr) {
            throw std::logic_error{"commit the unsized write before reserving"};
        }

        auto *block = block_.pointer().base_block();
        const auto begin = position_ + details::alignment_padding(position_, alignment);
        const auto block_end = buffer::block_end(block);
        if (begin > block_end || size > block_end - begin) {
            return reserve_in_next_block(size, alignment);
        }

        const auto reservation_end = begin + size;
        position_ = reservation_end;
        return {block, begin, reservation_end};
    }

  private:
    void advance_to_next_block(std::size_t required_capacity) {
        auto *current = pending_tail_ == nullptr ? block_.get<details::single_writer_block>() : pending_tail_;
        if (current == nullptr) {
            throw std::length_error{"contiguous buffer has insufficient capacity"};
        }

        const auto preferred = current->preferred_capacity;
        auto *next = details::single_writer_block::create(current->allocator, std::max(preferred, required_capacity),
                                                          preferred);
        if (pending_tail_ == nullptr) {
            starting_block_end_ = position_;
            pending_first_ = details::block_reference::adopt(next);
        } else {
            current->sealed_end = position_;
            current->committed.store(position_, std::memory_order_relaxed);
            current->next.store(next, std::memory_order_relaxed);
        }

        pending_tail_ = next;
        position_ = next->data_offset();
    }

    buffer_reservation reserve_in_next_block(std::size_t size, std::size_t alignment) {
        auto *single = block_.get<details::single_writer_block>();
        if (single == nullptr) {
            throw std::length_error{"contiguous buffer has insufficient capacity"};
        }

        const auto preferred = single->preferred_capacity;
        const auto next_position = sizeof(details::single_writer_block);
        const auto padding = details::alignment_padding(next_position, alignment);
        auto *next = details::single_writer_block::create(single->allocator, std::max(preferred, padding + size),
                                                          preferred);
        single->sealed_end = position_;
        single->next.store(next, std::memory_order_release);
        block_ = details::block_reference{next};

        const auto begin = next->data_offset() + details::alignment_padding(next->data_offset(), alignment);
        const auto reservation_end = begin + size;
        position_ = reservation_end;
        return {next, begin, reservation_end};
    }

    details::block_reference block_;
    details::block_reference pending_first_;
    details::single_writer_block *pending_tail_{};
    std::size_t position_{};
    std::size_t starting_block_end_{};

    friend class buffer_reader;
};

class concurrent_buffer_reservation {
  public:
    // A reservation must not outlive its writer. Do not overlap reservations from the same writer.
    concurrent_buffer_reservation(const concurrent_buffer_reservation &) = delete;
    concurrent_buffer_reservation &operator=(const concurrent_buffer_reservation &) = delete;
    concurrent_buffer_reservation(concurrent_buffer_reservation &&other) noexcept
        : block_{std::exchange(other.block_, nullptr)}, sequence_{other.sequence_}, begin_{other.begin_},
          cursor_{other.cursor_}, end_{other.end_} {}
    concurrent_buffer_reservation &operator=(concurrent_buffer_reservation &&) = delete;

    std::span<std::byte> bytes() const noexcept {
        if (block_ == nullptr) {
            return {};
        }
        auto *block = block_;
        return {block->at(block->data_offset() + begin_), end_ - begin_};
    }

    void write_bytes(std::span<const std::byte> bytes) {
        if (bytes.empty()) {
            return;
        }
        require_space(bytes.size());
        std::memcpy(block_->at(block_->data_offset() + cursor_), bytes.data(), bytes.size());
        cursor_ += static_cast<std::uint32_t>(bytes.size());
    }

    template <typename T, typename... Args> T &emplace(Args &&...args) {
        details::require_supported_alignment(alignof(T));
        const auto offset = block_->data_offset() + cursor_;
        const auto begin = cursor_ + static_cast<std::uint32_t>(details::alignment_padding(offset, alignof(T)));
        require_space(begin - cursor_ + sizeof(T));
        auto *value = std::construct_at(reinterpret_cast<T *>(block_->at(block_->data_offset() + begin)),
                                        std::forward<Args>(args)...);
        cursor_ = begin + static_cast<std::uint32_t>(sizeof(T));
        return *value;
    }

    template <typename T> auto &write(T &&value) {
        return emplace<std::remove_cvref_t<T>>(std::forward<T>(value));
    }

    void commit() {
        if (block_ == nullptr) {
            throw std::logic_error{"buffer reservation already committed"};
        }
        block_->commit(sequence_, end_);
        block_ = nullptr;
    }

  private:
    concurrent_buffer_reservation(details::concurrent_writer_block *block, std::uint32_t sequence, std::uint32_t begin,
                                  std::uint32_t end)
        : block_{block}, sequence_{sequence}, begin_{begin}, cursor_{begin}, end_{end} {}

    void require_space(std::size_t size) const {
        if (block_ == nullptr || size > end_ - cursor_) {
            throw std::length_error{"buffer reservation has insufficient space"};
        }
    }

    details::concurrent_writer_block *block_{};
    std::uint32_t sequence_{};
    std::uint32_t begin_{};
    std::uint32_t cursor_{};
    std::uint32_t end_{};
    friend class concurrent_buffer_writer;
};

class concurrent_buffer_writer {
  public:
    // Use one thread per writer and commit each reservation before making another. For a late writer, pass
    // reader.current_block() to release consumed prefixes.
    explicit concurrent_buffer_writer(buffer buffer) {
        if (buffer.control_.get<details::concurrent_writer_block>() == nullptr || buffer.end_ != 0) {
            throw std::invalid_argument{"concurrent_buffer_writer requires a concurrent buffer chain"};
        }
        block_ = details::block_reference::adopt(buffer.detach_block());
    }

    concurrent_buffer_writer(const concurrent_buffer_writer &) = delete;
    concurrent_buffer_writer &operator=(const concurrent_buffer_writer &) = delete;
    concurrent_buffer_writer(concurrent_buffer_writer &&) = delete;
    concurrent_buffer_writer &operator=(concurrent_buffer_writer &&) = delete;

    concurrent_buffer_reservation reserve(std::size_t size, std::size_t alignment = 1) {
        if (size == 0) {
            throw std::invalid_argument{"buffer reservation must be positive"};
        }
        details::require_supported_alignment(alignment);
        if (size > UINT32_MAX) {
            throw std::length_error{"a concurrent reservation must fit a 32-bit block position"};
        }
        while (true) {
            auto *block = block_.get<details::concurrent_writer_block>();
            if (details::concurrent_writer_block::mark_is_sealed(
                    block->control.reservation_mark.load(std::memory_order_acquire))) {
                advance_to_next(size, alignment);
                continue;
            }

            auto sequence = std::uint32_t{};
            auto begin = std::uint32_t{};
            auto end = std::uint32_t{};
            if (block->try_reserve(size, alignment, sequence, begin, end)) {
                return {block, sequence, begin, end};
            }
            advance_to_next(size, alignment);
        }
    }

  private:
    void advance_to_next(std::size_t size, std::size_t alignment) {
        auto *next = block_.get<details::concurrent_writer_block>()->next_for_reservation(size, alignment);
        if (next != nullptr) {
            block_ = details::block_reference{next};
        }
    }
    details::block_reference block_;
};

class buffer_reader {
  public:
    explicit buffer_reader(buffer buffer) {
        const auto type = buffer.type();
        const auto begin = buffer.data_begin();
        const auto end = buffer.end_;
        block_ = details::block_reference::adopt(buffer.detach_block());
        if (type == buffer_type::contiguous) {
            mode_ = mode::contiguous;
            position_ = begin;
            auto *block =
                block_.pointer().visit([](auto *value) { return static_cast<details::buffer_block *>(value); });
            limit_ = end == 0 ? sizeof(details::buffer_block) + block->capacity : end;
        } else {
            mode_ = mode::chain;
            position_ = begin;
        }
    }

    buffer_reader &operator=(const buffer_reader &) = delete;
    buffer_reader(buffer_reader &&) = delete;
    buffer_reader &operator=(buffer_reader &&) = delete;

    buffer_reader fork() const {
        if (mode_ == mode::pending_write) {
            throw std::logic_error{"cannot fork a reader over uncommitted buffer data"};
        }
        return buffer_reader{*this};
    }

    std::span<const std::byte> readable() {
        if (mode_ == mode::contiguous) {
            auto *block = block_.pointer().base_block();
            const auto end = std::min(limit_, block->committed.load(std::memory_order_acquire));
            return {block->at(position_), end > position_ ? end - position_ : 0};
        }

        while (block_) {
            auto *block = block_.pointer().base_block();
            if (mode_ == mode::pending_write) {
                const auto &writer = *writer_;
                const auto *root = writer.block_.pointer().base_block();
                const auto end = block == root ? (writer.pending_tail_ == nullptr ? writer.position_
                                                                                 : writer.starting_block_end_)
                                               : (block == writer.pending_tail_ ? writer.position_
                                                                                : block_.pointer().sealed_end());
                if (position_ < end) {
                    return {block->at(position_), end - position_};
                }
                if (writer.pending_tail_ == nullptr || block == writer.pending_tail_) {
                    return {};
                }

                const auto next = block == root ? writer.pending_first_.pointer() : block_.pointer().next_block();
                if (!next) {
                    return {};
                }

                next.visit([](auto *value) { value->retain(); });
                block_ = details::block_reference::adopt(next);
                position_ = next.data_offset();
                continue;
            }

            const auto end = block->committed.load(std::memory_order_acquire);
            if (position_ < end) {
                return {block->at(position_), end - position_};
            }

            const auto next = block_.pointer().next_block();
            if (!next || end < block_.pointer().sealed_end()) {
                return {};
            }

            next.visit([](auto *value) { value->retain(); });
            block_ = details::block_reference::adopt(next);
            position_ = next.data_offset();
        }
        return {};
    }

    buffer current_buffer() {
        const auto available = readable();
        if (available.empty()) {
            throw std::logic_error{"the reader has no committed bytes"};
        }
        return buffer{block_.pointer(), position_, position_ + available.size()};
    }

    // The returned handle resumes a reader at this position; writers start at the block's write mark.
    buffer current_block() { return buffer{block_.pointer(), position_, 0}; }

    void consume(std::size_t size) {
        while (size != 0) {
            const auto available = readable();
            if (available.empty()) {
                throw std::out_of_range{"cannot consume bytes that are not readable"};
            }

            const auto count = std::min(size, available.size());
            position_ += count;
            size -= count;
        }
    }

    void read_bytes(std::span<std::byte> destination) {
        while (!destination.empty()) {
            const auto available = readable();
            if (available.empty()) {
                throw std::out_of_range{"buffer reader has insufficient bytes"};
            }

            const auto count = std::min(destination.size(), available.size());
            std::memcpy(destination.data(), available.data(), count);
            position_ += count;
            destination = destination.subspan(count);
        }
    }

    template <typename T> T read_raw() {
        static_assert(std::is_trivially_copyable_v<T>);
        auto value = T{};
        read_bytes(std::as_writable_bytes(std::span{&value, std::size_t{1}}));
        return value;
    }

    template <typename T> const T &peek() {
        auto end = std::size_t{};
        return *value_at_cursor<T>(end);
    }

    // Advance over a typed object without destroying it. The object must be destroyed separately.
    template <typename T> void consume() {
        auto end = std::size_t{};
        value_at_cursor<T>(end);
        position_ = end;
    }

    template <typename T> T read() {
        auto end = std::size_t{};
        auto *value = value_at_cursor<T>(end);
        T result{std::move(*value)};
        std::destroy_at(value);
        position_ = end;
        return result;
    }

    template <typename T> void skip() {
        auto end = std::size_t{};
        auto *value = value_at_cursor<T>(end);
        std::destroy_at(value);
        position_ = end;
    }

  private:
    buffer_reader(const buffer_reader &other)
        : block_{other.block_}, position_{other.position_}, mode_{other.mode_} {
        if (mode_ == mode::pending_write) {
            writer_ = other.writer_;
        } else {
            limit_ = other.limit_;
        }
    }

    explicit buffer_reader(const buffer_writer &writer)
        : block_{writer.block_},
          position_{writer.block_.pointer().base_block()->committed.load(std::memory_order_relaxed)},
          mode_{mode::pending_write} {
        writer_ = &writer;
    }

    template <typename T> T *value_at_cursor(std::size_t &end) {
        details::require_supported_alignment(alignof(T));
        const auto available = readable();
        const auto begin = position_ + details::alignment_padding(position_, alignof(T));
        if (begin > position_ + available.size() || sizeof(T) > position_ + available.size() - begin) {
            throw std::out_of_range{"buffer reader has no complete typed value"};
        }

        end = begin + sizeof(T);
        auto *block = block_.pointer().base_block();
        return std::launder(reinterpret_cast<T *>(block->at(begin)));
    }

    enum class mode : std::uint8_t { contiguous, chain, pending_write };

    details::block_reference block_;
    std::size_t position_{};
    union {
        std::size_t limit_{};
        const buffer_writer *writer_;
    };
    mode mode_{mode::contiguous};

    friend class buffer_writer;
};

static_assert(sizeof(buffer_reader) ==
              ((sizeof(details::block_reference) + 2 * sizeof(std::size_t) + sizeof(std::uint8_t) +
                alignof(buffer_reader) - 1) /
               alignof(buffer_reader)) *
                  alignof(buffer_reader));

template <typename Cleanup> void buffer_writer::rollback(Cleanup &&cleanup) {
    try {
        auto reader = buffer_reader{*this};
        std::invoke(std::forward<Cleanup>(cleanup), reader);
    } catch (...) {
        rollback();
        throw;
    }
    rollback();
}

using concurrent_buffer_reader = buffer_reader;

} // namespace bg::util
