#pragma once

#include <bg/allocator.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg::util::details {

inline void require_supported_alignment(std::size_t alignment) {
    if (alignment == 1) {
        return;
    }
    if (!std::has_single_bit(alignment) || alignment > alignof(std::max_align_t)) {
        throw std::invalid_argument{"buffer alignment must be a power of two no greater than max_align_t"};
    }
}

inline std::size_t alignment_padding(std::size_t offset, std::size_t alignment) noexcept {
    return alignment == 1 ? 0 : (alignment - (offset & (alignment - 1))) & (alignment - 1);
}

class block_pointer;

struct alignas(std::max_align_t) allocation_control {
    explicit allocation_control(bg::allocator allocator) : allocator{allocator} {}

    void retain() noexcept { references.fetch_add(1, std::memory_order_relaxed); }

    bg::allocator allocator;
    std::atomic<std::size_t> references{1};
};

struct buffer_block : allocation_control {
    buffer_block(bg::allocator allocator, std::size_t capacity, std::size_t committed)
        : allocation_control{allocator}, capacity{capacity}, committed{committed} {}

    static buffer_block *create(bg::allocator allocator, std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument{"buffer capacity must be positive"};
        }
        if (capacity > std::numeric_limits<std::size_t>::max() - sizeof(buffer_block)) {
            throw std::length_error{"buffer allocation size overflows"};
        }

        auto *storage = allocator.allocate_bytes(sizeof(buffer_block) + capacity, alignof(buffer_block));
        return std::construct_at(static_cast<buffer_block *>(storage), allocator, capacity, sizeof(buffer_block));
    }

    std::byte *at(std::size_t offset) noexcept { return reinterpret_cast<std::byte *>(this) + offset; }
    const std::byte *at(std::size_t offset) const noexcept {
        return reinterpret_cast<const std::byte *>(this) + offset;
    }

    block_pointer take_next() noexcept;

    const std::size_t capacity;
    std::atomic<std::size_t> committed;
};

struct single_writer_block : buffer_block {
    single_writer_block(bg::allocator allocator, std::size_t capacity, std::size_t committed,
                        std::size_t preferred_capacity)
        : buffer_block{allocator, capacity, committed}, preferred_capacity{preferred_capacity} {}

    static single_writer_block *create(bg::allocator allocator, std::size_t payload_capacity,
                                       std::size_t preferred_capacity) {
        constexpr auto data_offset = sizeof(single_writer_block);
        constexpr auto prefix = data_offset - sizeof(buffer_block);
        if (payload_capacity == 0) {
            throw std::invalid_argument{"buffer block capacity must be positive"};
        }
        if (payload_capacity > std::numeric_limits<std::size_t>::max() - data_offset) {
            throw std::length_error{"buffer allocation size overflows"};
        }

        auto *storage = allocator.allocate_bytes(data_offset + payload_capacity, alignof(single_writer_block));
        return std::construct_at(static_cast<single_writer_block *>(storage), allocator, prefix + payload_capacity,
                                 data_offset, preferred_capacity);
    }

    single_writer_block *next_block() const noexcept { return next.load(std::memory_order_acquire); }
    single_writer_block *take_next() noexcept { return next.exchange(nullptr, std::memory_order_acq_rel); }
    std::size_t data_offset() const noexcept { return sizeof(single_writer_block); }

    std::atomic<single_writer_block *> next{};
    union {
        std::size_t preferred_capacity;
        std::size_t sealed_end;
    };
};

struct concurrent_block_control {
    std::uint32_t block_capacity{};
    std::uint32_t ring_capacity{};
    std::atomic<std::uint64_t> reservation_mark{};
    std::atomic<std::uint32_t> published{};
};

struct commit_slot {
    std::atomic<std::uint64_t> value{};
};

struct concurrent_writer_block : buffer_block {
    static constexpr std::uint64_t sealed_bit = std::uint64_t{1} << 63;
    static constexpr std::uint32_t sequence_mask = (std::uint32_t{1} << 31) - 1;
    static constexpr std::uint32_t publishing_bit = std::uint32_t{1} << 31;
    static constexpr std::uint32_t maximum_sequence = sequence_mask;
    static constexpr std::uint64_t empty_commit_value = std::uint64_t{UINT32_MAX} << 32;

    concurrent_writer_block(bg::allocator allocator, std::size_t capacity, std::size_t committed,
                            std::size_t block_capacity, std::size_t ring_capacity)
        : buffer_block{allocator, capacity, committed} {
        control.block_capacity = static_cast<std::uint32_t>(block_capacity);
        control.ring_capacity = static_cast<std::uint32_t>(ring_capacity);
    }

    static std::size_t data_offset(std::size_t ring_capacity) {
        constexpr auto fixed_prefix = sizeof(concurrent_writer_block);
        if (ring_capacity > (std::numeric_limits<std::size_t>::max() - fixed_prefix) / sizeof(commit_slot)) {
            throw std::length_error{"buffer commit ring allocation size overflows"};
        }
        return fixed_prefix + ring_capacity * sizeof(commit_slot);
    }

    static concurrent_writer_block *create(bg::allocator allocator, std::size_t payload_capacity,
                                           std::size_t block_capacity, std::size_t ring_capacity) {
        const auto offset = data_offset(ring_capacity);
        if (payload_capacity == 0) {
            throw std::invalid_argument{"buffer capacity must be positive"};
        }
        if (payload_capacity > std::numeric_limits<std::size_t>::max() - offset) {
            throw std::length_error{"buffer allocation size overflows"};
        }

        const auto capacity = offset - sizeof(buffer_block) + payload_capacity;
        auto *storage = allocator.allocate_bytes(offset + payload_capacity, alignof(concurrent_writer_block));
        auto *block = std::construct_at(static_cast<concurrent_writer_block *>(storage), allocator, capacity, offset,
                                        block_capacity, ring_capacity);
        for (auto index = std::size_t{}; index < ring_capacity; ++index) {
            std::construct_at(&block->commit_slots()[index], empty_commit_value);
        }
        return block;
    }

    concurrent_writer_block *next_block() const noexcept { return next.load(std::memory_order_acquire); }
    concurrent_writer_block *take_next() noexcept { return next.exchange(nullptr, std::memory_order_acq_rel); }
    std::size_t data_offset() const noexcept {
        return sizeof(concurrent_writer_block) + control.ring_capacity * sizeof(commit_slot);
    }
    std::size_t sealed_end() const noexcept {
        const auto mark = control.reservation_mark.load(std::memory_order_acquire);
        return mark_is_sealed(mark) ? data_offset() + mark_position(mark) : 0;
    }

    commit_slot *commit_slots() noexcept {
        return reinterpret_cast<commit_slot *>(at(sizeof(concurrent_writer_block)));
    }

    std::uint64_t make_reservation_mark(std::uint32_t position, std::uint32_t sequence,
                                        bool sealed = false) const noexcept {
        return (static_cast<std::uint64_t>(sequence) << 32) | position | (sealed ? sealed_bit : 0);
    }
    static std::uint32_t mark_position(std::uint64_t mark) noexcept { return static_cast<std::uint32_t>(mark); }
    static std::uint32_t mark_sequence(std::uint64_t mark) noexcept {
        return static_cast<std::uint32_t>((mark >> 32) & sequence_mask);
    }
    static bool mark_is_sealed(std::uint64_t mark) noexcept { return (mark & sealed_bit) != 0; }

    static std::uint64_t make_commit_value(std::uint32_t sequence, std::uint32_t end) noexcept {
        return (static_cast<std::uint64_t>(sequence) << 32) | end;
    }
    static std::uint32_t commit_sequence(std::uint64_t value) noexcept {
        return static_cast<std::uint32_t>(value >> 32);
    }
    static std::uint32_t commit_end(std::uint64_t value) noexcept { return static_cast<std::uint32_t>(value); }

    bool needs_rollover(std::uint64_t mark, std::size_t size, std::size_t alignment) const noexcept {
        const auto sequence = mark_sequence(mark);
        const auto position = mark_position(mark);
        const auto published_count = control.published.load(std::memory_order_acquire) & sequence_mask;
        const auto payload_capacity = capacity - (data_offset() - sizeof(buffer_block));
        const auto padding = alignment_padding(data_offset() + position, alignment);
        return mark_is_sealed(mark) || sequence == maximum_sequence ||
               sequence - published_count >= control.ring_capacity || padding > payload_capacity - position ||
               size > payload_capacity - position - padding;
    }

    bool try_reserve(std::size_t size, std::size_t alignment, std::uint32_t &sequence, std::uint32_t &begin,
                     std::uint32_t &end) noexcept {
        auto mark = control.reservation_mark.load(std::memory_order_acquire);
        if (mark_is_sealed(mark)) {
            return false;
        }

        sequence = mark_sequence(mark);
        const auto position = mark_position(mark);
        const auto payload_capacity = capacity - (data_offset() - sizeof(buffer_block));
        const auto padding = alignment_padding(data_offset() + position, alignment);
        if (padding > payload_capacity - position || size > payload_capacity - position - padding) {
            return false;
        }
        begin = static_cast<std::uint32_t>(position + padding);
        const auto published_count = control.published.load(std::memory_order_acquire) & sequence_mask;
        if (sequence == maximum_sequence || sequence - published_count >= control.ring_capacity) {
            return false;
        }

        end = begin + static_cast<std::uint32_t>(size);
        return control.reservation_mark.compare_exchange_weak(mark, make_reservation_mark(end, sequence + 1),
                                                              std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void commit(std::uint32_t sequence, std::uint32_t end) noexcept {
        auto &slot = commit_slots()[sequence & (control.ring_capacity - 1)];
        slot.value.store(make_commit_value(sequence, end), std::memory_order_release);
        publish_ready();
    }

    void publish_ready() noexcept {
        auto *slots = commit_slots();
        while (true) {
            auto state = control.published.load(std::memory_order_acquire);
            if ((state & publishing_bit) != 0) {
                control.published.wait(state, std::memory_order_acquire);
                continue;
            }

            const auto sequence = state & sequence_mask;
            auto &slot = slots[sequence & (control.ring_capacity - 1)];
            const auto value = slot.value.load(std::memory_order_acquire);
            if (commit_sequence(value) != sequence) {
                return;
            }
            if (!control.published.compare_exchange_weak(state, state | publishing_bit, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                continue;
            }

            committed.store(data_offset() + commit_end(value), std::memory_order_release);
            control.published.store(sequence + 1, std::memory_order_release);
            control.published.notify_all();
        }
    }

    bool seal_if_needed(std::size_t size, std::size_t alignment) noexcept {
        auto mark = control.reservation_mark.load(std::memory_order_acquire);
        while (!mark_is_sealed(mark)) {
            if (!needs_rollover(mark, size, alignment)) {
                return false;
            }
            if (control.reservation_mark.compare_exchange_weak(mark, mark | sealed_bit, std::memory_order_acq_rel,
                                                               std::memory_order_acquire)) {
                return true;
            }
        }
        return true;
    }

    concurrent_writer_block *next_for_reservation(std::size_t size, std::size_t alignment);

    std::atomic<concurrent_writer_block *> next{};
    concurrent_block_control control;
};

static_assert(std::is_trivially_destructible_v<concurrent_block_control>);
static_assert(std::is_trivially_destructible_v<commit_slot>);

class block_pointer {
    using storage_type = variant_ptr<buffer_block, single_writer_block, concurrent_writer_block>;

  public:
    block_pointer() = default;
    block_pointer(std::nullptr_t) noexcept {}
    template <typename Block> block_pointer(Block *block) noexcept : value_{block} {}
    template <typename Block> block_pointer &operator=(Block *block) noexcept {
        value_ = block;
        return *this;
    }
    explicit operator bool() const noexcept { return static_cast<bool>(value_); }
    template <typename Block> Block *get() const noexcept { return value_.template get<Block>(); }
    std::size_t data_offset() const noexcept;
    block_pointer next_block() const noexcept;
    std::size_t sealed_end() const noexcept;
    template <typename Visitor> decltype(auto) visit(Visitor &&visitor) const {
        return value_.visit(std::forward<Visitor>(visitor));
    }
    buffer_block *base_block() const noexcept {
        return visit([](auto *block) { return static_cast<buffer_block *>(block); });
    }

  private:
    storage_type value_;
};

inline block_pointer buffer_block::take_next() noexcept {
    return {};
}

class block_reference {
  public:
    block_reference() = default;
    template <typename Block> explicit block_reference(Block *block) : pointer_{block} { retain(); }
    template <typename Block> static block_reference adopt(Block *block) {
        block_reference result;
        result.pointer_ = block;
        return result;
    }
    static block_reference adopt(block_pointer pointer) {
        block_reference result;
        result.pointer_ = pointer;
        return result;
    }

    block_reference(const block_reference &other) : pointer_{other.pointer_} { retain(); }
    block_reference(block_reference &&other) noexcept : pointer_{std::exchange(other.pointer_, block_pointer{})} {}
    block_reference &operator=(const block_reference &other) {
        if (this != &other) {
            auto copy = other;
            swap(copy);
        }
        return *this;
    }
    block_reference &operator=(block_reference &&other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = std::exchange(other.pointer_, block_pointer{});
        }
        return *this;
    }
    ~block_reference() { reset(); }

    block_pointer pointer() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return static_cast<bool>(pointer_); }
    template <typename Block> Block *get() const noexcept { return pointer_.template get<Block>(); }
    void relinquish() noexcept { pointer_ = block_pointer{}; }
    void swap(block_reference &other) noexcept { std::swap(pointer_, other.pointer_); }
    void reset() noexcept {
        auto block = std::exchange(pointer_, block_pointer{});
        while (block) {
            block = block.visit([](auto *value) -> block_pointer {
                if (value->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                    return {};
                }

                auto next = value->take_next();
                auto allocator = value->allocator;
                const auto size = sizeof(buffer_block) + value->capacity;
                using block_type = std::remove_pointer_t<decltype(value)>;
                std::destroy_at(value);
                allocator.deallocate_bytes(value, size, alignof(block_type));
                return next;
            });
        }
    }

  private:
    void retain() noexcept {
        if (pointer_) {
            pointer_.visit([](auto *block) { block->retain(); });
        }
    }
    block_pointer pointer_;
};

inline std::size_t block_pointer::data_offset() const noexcept {
    return visit([](auto *block) {
        using block_type = std::remove_pointer_t<decltype(block)>;
        if constexpr (std::is_same_v<block_type, buffer_block>) {
            return sizeof(buffer_block);
        } else {
            return block->data_offset();
        }
    });
}

inline block_pointer block_pointer::next_block() const noexcept {
    return visit([](auto *block) -> block_pointer {
        using block_type = std::remove_pointer_t<decltype(block)>;
        if constexpr (std::is_same_v<block_type, buffer_block>) {
            return {};
        } else {
            return block->next_block();
        }
    });
}

inline std::size_t block_pointer::sealed_end() const noexcept {
    return visit([](auto *block) {
        using block_type = std::remove_pointer_t<decltype(block)>;
        if constexpr (std::is_same_v<block_type, single_writer_block>) {
            return block->sealed_end;
        } else if constexpr (std::is_same_v<block_type, concurrent_writer_block>) {
            return block->sealed_end();
        } else {
            return std::size_t{};
        }
    });
}

inline concurrent_writer_block *concurrent_writer_block::next_for_reservation(std::size_t size,
                                                                               std::size_t alignment) {
    if (!seal_if_needed(size, alignment)) {
        return nullptr;
    }
    if (auto *existing = next.load(std::memory_order_acquire)) {
        return existing;
    }

    const auto padding = alignment_padding(data_offset(control.ring_capacity), alignment);
    if (size > UINT32_MAX - padding) {
        throw std::length_error{"concurrent reservation and alignment padding exceed a block position"};
    }
    auto *candidate = create(allocator, std::max<std::size_t>(control.block_capacity, padding + size),
                             control.block_capacity, control.ring_capacity);
    auto owner = block_reference::adopt(candidate);
    auto *expected = static_cast<concurrent_writer_block *>(nullptr);
    if (next.compare_exchange_strong(expected, candidate, std::memory_order_acq_rel, std::memory_order_acquire)) {
        owner.relinquish();
        return candidate;
    }
    return expected;
}

inline concurrent_writer_block *new_concurrent_writer_block(bg::allocator allocator, std::size_t block_capacity,
                                                            std::size_t ring_capacity) {
    if (block_capacity == 0 || block_capacity > UINT32_MAX) {
        throw std::invalid_argument{"buffer blocks must have a positive 32-bit capacity"};
    }
    if (ring_capacity == 0 || ring_capacity > static_cast<std::size_t>(concurrent_writer_block::maximum_sequence) + 1 ||
        !std::has_single_bit(ring_capacity)) {
        throw std::invalid_argument{"commit ring capacity must be a power of two that fits a block sequence"};
    }
    return concurrent_writer_block::create(allocator, block_capacity, block_capacity, ring_capacity);
}

using buffer_control = variant_ptr<buffer_block, single_writer_block, concurrent_writer_block>;

} // namespace bg::util::details
