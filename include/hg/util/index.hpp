#pragma once

#include <bg/util/byte_copy.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <utility>

namespace bg::util {

inline constexpr auto dynamic_index_extent = std::numeric_limits<std::uint32_t>::max();

template <std::uint32_t Extent = dynamic_index_extent> class index;

namespace details {

template <typename Index> class index_iterator {
  public:
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::uint32_t;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = value_type;

    index_iterator() noexcept = default;

    value_type operator*() const noexcept {
        return word_ * bits_per_word + static_cast<value_type>(std::countr_zero(remaining_));
    }

    index_iterator &operator++() noexcept {
        remaining_ &= remaining_ - 1;
        advance();

        return *this;
    }

    index_iterator operator++(int) noexcept {
        auto previous = *this;
        ++*this;

        return previous;
    }

    bool operator==(const index_iterator &) const noexcept = default;

  private:
    template <std::uint32_t> friend class bg::util::index;

    static constexpr auto bits_per_word = std::uint32_t{64};

    index_iterator(const Index &value, std::uint32_t word) noexcept : index_{&value}, word_{word} {
        if (word_ < index_->word_count()) {
            remaining_ = index_->word(word_);
        }
        advance();
    }

    void advance() noexcept {
        while (remaining_ == 0 && word_ < index_->word_count()) {
            ++word_;
            if (word_ < index_->word_count()) {
                remaining_ = index_->word(word_);
            }
        }
    }

    const Index *index_{};
    std::uint32_t word_{};
    std::uint64_t remaining_{};
};

constexpr std::uint64_t low_bits(std::uint32_t count) noexcept {
    if (count == 0) {
        return 0;
    }
    if (count == 64) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    return (std::uint64_t{1} << count) - 1;
}

template <typename ReadWord, typename WriteBits>
void relocate_bits(std::uint32_t source, std::uint32_t destination, std::uint32_t count,
                   ReadWord &&read_word, WriteBits &&write_bits) noexcept {
    constexpr auto bits_per_word = std::uint64_t{64};
    const auto source_end = std::uint64_t{source} + count;
    const auto destination_end = std::uint64_t{destination} + count;

    if (destination < source) {
        auto copied = std::uint64_t{};
        while (copied < count) {
            const auto source_position = std::uint64_t{source} + copied;
            const auto destination_position = std::uint64_t{destination} + copied;
            const auto source_offset = source_position % bits_per_word;
            const auto destination_offset = destination_position % bits_per_word;
            const auto chunk_size =
                std::min(std::uint64_t{count} - copied,
                         std::min(bits_per_word - source_offset, bits_per_word - destination_offset));
            const auto bits = read_word(static_cast<std::uint32_t>(source_position / bits_per_word)) >>
                              source_offset;
            write_bits(static_cast<std::uint32_t>(destination_position), bits,
                       static_cast<std::uint32_t>(chunk_size));
            copied += chunk_size;
        }
    } else {
        auto remaining = std::uint64_t{count};
        while (remaining != 0) {
            const auto source_chunk_end = std::uint64_t{source} + remaining;
            const auto destination_chunk_end = std::uint64_t{destination} + remaining;
            const auto source_offset = (source_chunk_end - 1) % bits_per_word;
            const auto destination_offset = (destination_chunk_end - 1) % bits_per_word;
            const auto chunk_size = std::min(remaining, std::min(source_offset + 1, destination_offset + 1));
            const auto source_position = source_chunk_end - chunk_size;
            const auto destination_position = destination_chunk_end - chunk_size;
            const auto bits = read_word(static_cast<std::uint32_t>(source_position / bits_per_word)) >>
                              (source_position % bits_per_word);
            write_bits(static_cast<std::uint32_t>(destination_position), bits,
                       static_cast<std::uint32_t>(chunk_size));
            remaining -= chunk_size;
        }
    }

    auto clear_start = std::uint64_t{};
    auto clear_end = std::uint64_t{};
    if (destination < source) {
        clear_start = std::max(std::uint64_t{source}, destination_end);
        clear_end = source_end;
    } else {
        clear_start = source;
        clear_end = std::min(source_end, std::uint64_t{destination});
    }

    while (clear_start < clear_end) {
        const auto chunk_size = std::min(clear_end - clear_start, bits_per_word - clear_start % bits_per_word);
        write_bits(static_cast<std::uint32_t>(clear_start), 0, static_cast<std::uint32_t>(chunk_size));
        clear_start += chunk_size;
    }
}

} // namespace details

// Stores a runtime-sized bit index in one word while its size does not exceed 56 bits.
// The caller supplies one memory resource for growth and release.
template <> class index<dynamic_index_extent> {
    struct alignas(std::uint64_t) AllocatedStorage {
        std::uint32_t size;
        std::uint32_t capacity;
    };

  public:
    using size_type = std::uint32_t;
    using iterator = details::index_iterator<index>;

    static constexpr auto inline_capacity = size_type{56};
    static constexpr bool supports_byte_copy_initialization = true;

    index() = default;

    index(const index &) = delete;
    index &operator=(const index &) = delete;
    index(index &&) = delete;
    index &operator=(index &&) = delete;

    bool set(size_type position, std::pmr::memory_resource &memory) {
        if (position == dynamic_index_extent) {
            throw std::length_error{"index position exceeds the supported range"};
        }

        grow(position + 1, memory);
        const auto mask = std::uint64_t{1} << (position % bits_per_word);
        if (allocated()) {
            auto &target = allocated_words()[position / bits_per_word];
            const auto inserted = (target & mask) == 0;
            target |= mask;

            return inserted;
        }

        const auto shifted_mask = mask << inline_bits_shift;
        const auto inserted = (raw_ & shifted_mask) == 0;
        raw_ |= shifted_mask;

        return inserted;
    }

    bool reset(size_type position) noexcept {
        if (position >= size()) {
            return false;
        }

        const auto mask = std::uint64_t{1} << (position % bits_per_word);
        if (allocated()) {
            auto &target = allocated_words()[position / bits_per_word];
            const auto removed = (target & mask) != 0;
            target &= ~mask;

            return removed;
        }

        const auto shifted_mask = mask << inline_bits_shift;
        const auto removed = (raw_ & shifted_mask) != 0;
        raw_ &= ~shifted_mask;

        return removed;
    }

    bool contains(size_type position) const noexcept {
        return position < size() &&
               (word(position / bits_per_word) & (std::uint64_t{1} << (position % bits_per_word))) != 0;
    }

    void grow(size_type new_size, std::pmr::memory_resource &memory) {
        if (new_size <= size()) {
            return;
        }

        if (!allocated() && new_size <= inline_capacity) {
            raw_ = (raw_ & ~inline_size_mask) | (std::uint64_t{new_size} << inline_size_shift);
            return;
        }

        if (allocated() && new_size <= storage()->capacity) {
            storage()->size = new_size;
            return;
        }

        grow_allocation(new_size, memory);
    }

    void trim(std::pmr::memory_resource &memory) noexcept {
        const auto new_size = last_set() + static_cast<size_type>(any());
        if (!allocated()) {
            raw_ = (inline_word() << inline_bits_shift) | (std::uint64_t{new_size} << inline_size_shift);
            return;
        }

        if (new_size <= inline_capacity) {
            const auto bits = word(0) & details::low_bits(new_size);
            release_allocation(memory);
            raw_ = (bits << inline_bits_shift) | (std::uint64_t{new_size} << inline_size_shift);
            return;
        }

        storage()->size = new_size;
    }

    void clear() noexcept {
        if (!allocated()) {
            raw_ &= inline_metadata_mask;
            return;
        }

        std::fill_n(allocated_words(), word_count(), std::uint64_t{});
    }

    // Moves [source, source + count) to destination without allocation and clears source-only bits. Size is
    // unchanged. Both ranges must fit in size(), and destination bits outside the source range must be clear.
    void relocate(size_type source, size_type destination, size_type count) noexcept {
        if (count == 0 || source == destination) {
            return;
        }

        assert(source <= size() && count <= size() - source);
        assert(destination <= size() && count <= size() - destination);

        details::relocate_bits(source, destination, count, [this](size_type word_index) { return word(word_index); },
                               [this](size_type position, std::uint64_t bits, size_type count) {
                                   const auto offset = position % bits_per_word;
                                   const auto mask = details::low_bits(count) << offset;
                                   const auto moved_bits = (bits << offset) & mask;
                                   if (allocated()) {
                                       auto &target = allocated_words()[position / bits_per_word];
                                       target = (target & ~mask) | moved_bits;
                                       return;
                                   }

                                   const auto shifted_mask = mask << inline_bits_shift;
                                   raw_ = (raw_ & ~shifted_mask) | (moved_bits << inline_bits_shift);
                               });
    }

    void release(std::pmr::memory_resource &memory) noexcept {
        if (allocated()) {
            release_allocation(memory);
        }
        raw_ = 0;
    }

    void swap(index &other) noexcept { std::swap(raw_, other.raw_); }

    size_type size() const noexcept { return allocated() ? storage()->size : inline_size(); }

    size_type capacity() const noexcept { return allocated() ? storage()->capacity : inline_capacity; }

    std::size_t count() const noexcept {
        auto result = std::size_t{};
        for (auto word_index = size_type{}; word_index < word_count(); ++word_index) {
            result += std::popcount(word(word_index));
        }

        return result;
    }

    bool any() const noexcept {
        for (auto word_index = size_type{}; word_index < word_count(); ++word_index) {
            if (word(word_index) != 0) {
                return true;
            }
        }

        return false;
    }

    bool empty() const noexcept { return !any(); }
    bool all() const noexcept { return count() == size(); }

    size_type first_set() const noexcept {
        return first_set(0);
    }

    size_type first_set(size_type start) const noexcept {
        if (start >= size()) {
            return size();
        }

        auto word_index = start / bits_per_word;
        auto bits = word(word_index) & (std::numeric_limits<std::uint64_t>::max() << (start % bits_per_word));
        while (true) {
            if (bits != 0) {
                return word_index * bits_per_word + static_cast<size_type>(std::countr_zero(bits));
            }

            ++word_index;
            if (word_index == word_count()) {
                return size();
            }
            bits = word(word_index);
        }
    }

    // Returns the first set position in either index at or after start. The maximum
    // index size is the no-match sentinel.
    size_type first_set_union(const index &other, size_type start = 0) const noexcept {
        const auto extent = std::max(size(), other.size());
        if (start >= extent) {
            return extent;
        }

        const auto words = std::max(word_count(), other.word_count());
        auto word_index = start / bits_per_word;
        const auto this_words = word_count();
        const auto other_words = other.word_count();
        auto bits = (word_index < this_words ? word(word_index) : 0) |
                    (word_index < other_words ? other.word(word_index) : 0);
        bits &= std::numeric_limits<std::uint64_t>::max() << (start % bits_per_word);

        while (true) {
            if (bits != 0) {
                return word_index * bits_per_word + static_cast<size_type>(std::countr_zero(bits));
            }

            ++word_index;
            if (word_index == words) {
                return extent;
            }
            bits = (word_index < this_words ? word(word_index) : 0) |
                   (word_index < other_words ? other.word(word_index) : 0);
        }
    }

    size_type first_unset(size_type start = 0) const noexcept {
        return first_unset(start, size());
    }

    // Searches [start, end). Positions after size() are unset. Returns end if no unset bit occurs in the range;
    // empty and reversed ranges also return end.
    size_type first_unset(size_type start, size_type end) const noexcept {
        if (start >= end) {
            return end;
        }

        const auto current_size = size();
        if (start >= current_size) {
            return start;
        }

        const auto search_end = std::min(end, current_size);
        const auto final_word = (search_end - 1) / bits_per_word;
        auto word_index = start / bits_per_word;
        auto available = ~word(word_index) & (std::numeric_limits<std::uint64_t>::max() << (start % bits_per_word));
        while (word_index < final_word) {
            if (available != 0) {
                return word_index * bits_per_word + static_cast<size_type>(std::countr_zero(available));
            }

            ++word_index;
            available = ~word(word_index);
        }

        const auto final_word_start = std::uint64_t{final_word} * bits_per_word;
        const auto final_word_bits = static_cast<size_type>(std::uint64_t{search_end} - final_word_start);
        available &= details::low_bits(final_word_bits);
        if (available != 0) {
            return final_word * bits_per_word + static_cast<size_type>(std::countr_zero(available));
        }

        return search_end;
    }

    iterator begin() const noexcept { return iterator{*this, 0}; }
    iterator end() const noexcept { return iterator{*this, word_count()}; }

  private:
    friend class details::index_iterator<index>;

    static constexpr auto allocated_tag = std::uint64_t{1};
    static constexpr auto inline_size_shift = size_type{1};
    static constexpr auto inline_size_mask = std::uint64_t{0x7f} << inline_size_shift;
    static constexpr auto inline_bits_shift = size_type{8};
    static constexpr auto inline_metadata_mask = (std::uint64_t{1} << inline_bits_shift) - 1;
    static constexpr auto bits_per_word = size_type{64};

    static_assert(alignof(AllocatedStorage) >= 2);

    bool allocated() const noexcept { return (raw_ & allocated_tag) != 0; }

    size_type inline_size() const noexcept {
        return static_cast<size_type>((raw_ & inline_size_mask) >> inline_size_shift);
    }

    std::uint64_t inline_word() const noexcept { return raw_ >> inline_bits_shift; }

    AllocatedStorage *storage() noexcept {
        return reinterpret_cast<AllocatedStorage *>(raw_ & ~allocated_tag);
    }

    const AllocatedStorage *storage() const noexcept {
        return reinterpret_cast<const AllocatedStorage *>(raw_ & ~allocated_tag);
    }

    std::uint64_t *allocated_words() noexcept { return reinterpret_cast<std::uint64_t *>(storage() + 1); }

    const std::uint64_t *allocated_words() const noexcept {
        return reinterpret_cast<const std::uint64_t *>(storage() + 1);
    }

    std::uint64_t word(size_type word_index) const noexcept {
        return allocated() ? allocated_words()[word_index] : inline_word();
    }

    size_type word_count() const noexcept { return static_cast<size_type>(word_count(size())); }

    size_type last_set() const noexcept {
        for (auto word_index = word_count(); word_index != 0; --word_index) {
            const auto bits = word(word_index - 1);
            if (bits != 0) {
                return (word_index - 1) * bits_per_word +
                       (bits_per_word - 1 - static_cast<size_type>(std::countl_zero(bits)));
            }
        }

        return 0;
    }

    static constexpr std::size_t word_count(size_type size) noexcept {
        return (static_cast<std::size_t>(size) + bits_per_word - 1) / bits_per_word;
    }

    static std::size_t allocation_size(size_type capacity) noexcept {
        return sizeof(AllocatedStorage) + word_count(capacity) * sizeof(std::uint64_t);
    }

    static size_type allocation_capacity(std::size_t bytes) noexcept {
        const auto bits = (bytes - sizeof(AllocatedStorage)) / sizeof(std::uint64_t) * bits_per_word;
        return static_cast<size_type>(std::min<std::size_t>(bits, dynamic_index_extent));
    }

    void grow_allocation(size_type new_size, std::pmr::memory_resource &memory) {
        constexpr auto allocation_block_size = std::size_t{64};
        const auto required = allocation_size(new_size);
        auto bytes = allocated() ? allocation_size(storage()->capacity) * 2 : allocation_block_size;
        bytes = std::max(bytes, (required + allocation_block_size - 1) / allocation_block_size * allocation_block_size);
        bytes = std::min(bytes, allocation_size(dynamic_index_extent));

        auto *new_storage = static_cast<AllocatedStorage *>(memory.allocate(bytes, alignof(AllocatedStorage)));
        const auto new_capacity = allocation_capacity(bytes);
        std::construct_at(new_storage, AllocatedStorage{new_size, new_capacity});
        auto *new_words = reinterpret_cast<std::uint64_t *>(new_storage + 1);
        std::fill_n(new_words, word_count(new_capacity), std::uint64_t{});

        if (allocated()) {
            std::copy_n(allocated_words(), word_count(), new_words);
            release_allocation(memory);
        } else {
            new_words[0] = inline_word();
        }

        raw_ = reinterpret_cast<std::uintptr_t>(new_storage) | allocated_tag;
    }

    void release_allocation(std::pmr::memory_resource &memory) noexcept {
        auto *current = storage();
        const auto bytes = allocation_size(current->capacity);
        std::destroy_at(current);
        memory.deallocate(current, bytes, alignof(AllocatedStorage));
    }

    std::uint64_t raw_{};
};

// Stores a compile-time-sized bit index without allocation.
template <std::uint32_t Extent> class index {
  public:
    using size_type = std::uint32_t;
    using iterator = details::index_iterator<index>;

    bool set(size_type position) {
        require_position(position);
        const auto mask = std::uint64_t{1} << (position % bits_per_word);
        auto &target = words_[position / bits_per_word];
        const auto inserted = (target & mask) == 0;
        target |= mask;

        return inserted;
    }

    bool reset(size_type position) {
        require_position(position);
        const auto mask = std::uint64_t{1} << (position % bits_per_word);
        auto &target = words_[position / bits_per_word];
        const auto removed = (target & mask) != 0;
        target &= ~mask;

        return removed;
    }

    bool contains(size_type position) const noexcept {
        return position < Extent &&
               (words_[position / bits_per_word] & (std::uint64_t{1} << (position % bits_per_word))) != 0;
    }

    void clear() noexcept { words_.fill(0); }

    // Moves [source, source + count) to destination without allocation and clears source-only bits. Size is
    // unchanged. Both ranges must fit in size(), and destination bits outside the source range must be clear.
    void relocate(size_type source, size_type destination, size_type count) noexcept {
        if (count == 0 || source == destination) {
            return;
        }

        assert(source <= Extent && count <= Extent - source);
        assert(destination <= Extent && count <= Extent - destination);

        details::relocate_bits(
            source, destination, count, [this](size_type word_index) { return words_[word_index]; },
            [this](size_type position, std::uint64_t bits, size_type count) {
                const auto offset = position % bits_per_word;
                const auto mask = details::low_bits(count) << offset;
                auto &target = words_[position / bits_per_word];
                target = (target & ~mask) | ((bits << offset) & mask);
            });
    }

    std::size_t count() const noexcept {
        auto result = std::size_t{};
        for (const auto word : words_) {
            result += std::popcount(word);
        }

        return result;
    }

    bool any() const noexcept {
        for (const auto word : words_) {
            if (word != 0) {
                return true;
            }
        }

        return false;
    }

    bool empty() const noexcept { return !any(); }
    bool all() const noexcept { return count() == Extent; }

    size_type first_set() const noexcept {
        return first_set(0);
    }

    size_type first_set(size_type start) const noexcept {
        if (start >= Extent) {
            return Extent;
        }

        auto word_index = start / bits_per_word;
        auto bits = words_[word_index] & (std::numeric_limits<std::uint64_t>::max() << (start % bits_per_word));
        while (true) {
            if (bits != 0) {
                return word_index * bits_per_word + static_cast<size_type>(std::countr_zero(bits));
            }

            ++word_index;
            if (word_index == word_count()) {
                return Extent;
            }
            bits = words_[word_index];
        }
    }

    static constexpr size_type size() noexcept { return Extent; }

    iterator begin() const noexcept { return iterator{*this, 0}; }
    iterator end() const noexcept { return iterator{*this, word_count()}; }

  private:
    friend class details::index_iterator<index>;

    static constexpr auto bits_per_word = size_type{64};
    static constexpr auto words = (Extent + bits_per_word - 1) / bits_per_word;

    static void require_position(size_type position) {
        if (position >= Extent) {
            throw std::out_of_range{"index position is out of range"};
        }
    }

    static constexpr size_type word_count() noexcept { return words; }

    std::uint64_t word(size_type word_index) const noexcept { return words_[word_index]; }

    std::array<std::uint64_t, words> words_{};
};

static_assert(sizeof(index<>) == sizeof(void *));
static_assert(byte_copy_initializable<index<>>);

} // namespace bg::util
